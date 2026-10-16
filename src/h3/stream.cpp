//
// Http3Stream: one HTTP/3 request/response exchange, shared by the server and the client.
// See anyhttp/h3/stream.hpp for the model; the role-specific ends of it live in
// h3/server.cpp and h3/client.cpp.
//
#include "anyhttp/h3/stream.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3/application.hpp"
#include "anyhttp/h3/common.hpp"
#include "anyhttp/quic/session.hpp"

#include <boost/beast/http/error.hpp>
#include <boost/system/detail/errc.hpp>

#include <charconv>

using namespace boost::asio;

namespace anyhttp::http3
{

// =================================================================================================

Http3Stream::Http3Stream(Http3Application& app, int64_t stream_id, quic::WriteMode mode)
   : QuicStream(app.session(), stream_id, mode), application(app)
{
}

Http3Stream::~Http3Stream()
{
   mlogd("\x1b[33mStream: dtor... \x1b[0m");
   detach_all(); // while the request line and fields are still there for the adapters to take
}

// =================================================================================================
// Outgoing body, through nghttp3
// =================================================================================================

void Http3Stream::on_writable()
{
   if (auto h3 = application.h3())
      nghttp3_conn_resume_stream(h3, id);
   session.wake_write();
}

// https://nghttp2.org/nghttp3/types.html#c.nghttp3_read_data_callback
nghttp3_ssize Http3Stream::data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags)
{
   if (veccnt == 0)
      return 0;

   //
   // nghttp3 takes what it is given once and keeps it, so it gets what it has not seen yet, and
   // holds on to it from then on. It picks up the rest by itself as packets are filled -- see
   // QuicSession::write_pkt(), which keeps feeding the same vec to ngtcp2_conn_writev_stream()
   // and advances nghttp3 by whatever went into the packet.
   //
   static_assert(sizeof(nghttp3_vec) == sizeof(ngtcp2_vec));
   bool fin = false;
   const auto n = pending_data(tx_offered, {reinterpret_cast<ngtcp2_vec*>(vec), veccnt}, fin);

   //
   // Nothing new, and the body goes on: block rather than returning 0 bytes -- returning 0
   // without NGHTTP3_DATA_FLAG_EOF would tell nghttp3 the body ended. There is nothing until the
   // caller starts the next write, or the chunk on its way has gone out, which both resume the
   // stream.
   //
   if (n == 0 && !fin)
      return NGHTTP3_ERR_WOULDBLOCK;

   uint64_t end = tx_offered;
   for (size_t i = 0; i < n; ++i)
      end += vec[i].len;
   if (fin)
      *pflags |= NGHTTP3_DATA_FLAG_EOF; // repeated, should nghttp3 ask again after the end
   on_offered(end, fin);
   return static_cast<nghttp3_ssize>(n);
}

void Http3Stream::on_write_acked(size_t n)
{
   //
   // n counts *body* bytes acknowledged on this stream -- nghttp3 accounts for the HTTP/3 framing
   // it puts around the body itself, so unlike ngtcp2's stream offsets these are exactly the
   // bytes the caller handed us.
   //
   if (n > 0)
      on_acked(tx_acked + n);
}

void Http3Stream::on_write_offered(size_t n)
{
   //
   // n is the number of bytes of *stream* data ngtcp2 just committed to a packet, which also
   // includes the HTTP/3 framing nghttp3 puts around the body -- the HEADERS frame ahead of any
   // body as much as each DATA frame header. Only bytes nghttp3 holds may be attributed: the
   // very first write pass after async_submit() drains the headers before there is any body
   // yet. Should a single packet straddle the header/body boundary, the count is a little
   // ahead, which only ever completes a staged write early -- its chunks live until they are
   // acknowledged, and those acknowledgements are exact.
   //
   if (n > 0)
      on_sent(tx_sent + std::min<uint64_t>(n, tx_offered - tx_sent));
}

// =================================================================================================
// Headers//
// =================================================================================================
// Headers
// =================================================================================================

void Http3Stream::on_header(std::string_view name, std::string_view value)
{
   //
   // Beyond the limit, fields are not stored any more. What happens to the stream is decided once
   // the field section is complete, see on_headers_complete() of the server and client streams.
   //
   if (header_limit_exceeded)
      return;

   header_size += header_field_size(name, value);
   if (header_size > application.max_header_size())
   {
      mlogw("header section exceeds {} bytes, ignoring the rest", application.max_header_size());
      header_limit_exceeded = true;
      received_headers.clear();
      return;
   }

   if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))
      received_headers.emplace_back(name, value);

   try
   {
      if (name.starts_with(':'))
      {
         on_pseudo_header(name, value);
         return;
      }

      if (name == "content-length")
      {
         size_t len = 0;
         if (std::from_chars(value.begin(), value.end(), len).ec == std::errc{})
            content_length = len;
      }

      fields.insert(name, value); // insert, not set: repeated fields must all be kept
   }
   catch (const std::exception& ex)
   {
      mlogw("ignoring invalid header: {} ({})", value, ex.what());
   }
}

void Http3Stream::on_end_headers()
{
   headers_received = true;
   on_headers_complete();
}

namespace
{
nghttp3_ssize stream_read_data(nghttp3_conn*, int64_t /*stream_id*/, nghttp3_vec* vec,
                               size_t veccnt, uint32_t* pflags, void* /*conn_user*/,
                               void* stream_user)
{
   auto s = static_cast<Http3Stream*>(stream_user);
   return s->data_reader(vec, veccnt, pflags);
}
} // namespace

bool Http3Stream::submit_headers(std::span<const nghttp3_nv> nva, bool is_request)
{
   auto* h3 = application.h3();
   if (!h3)
   {
      mloge("submit_headers: HTTP/3 layer is gone");
      return false;
   }

   //
   // nghttp3 pulls the body out of this stream through data_reader(), with the stream itself as
   // the per-stream user data it hands back.
   //
   nghttp3_data_reader dr{};
   dr.read_data = &stream_read_data;

   auto* nv = const_cast<nghttp3_nv*>(nva.data());
   if (is_request)
   {
      if (auto rv = nghttp3_conn_submit_request(h3, id, nv, nva.size(), &dr, this); rv != 0)
      {
         mloge("nghttp3_conn_submit_request: {}", nghttp3_strerror(rv));
         return false;
      }
   }
   else
   {
      if (auto rv = nghttp3_conn_set_stream_user_data(h3, id, this); rv != 0)
      {
         mloge("nghttp3_conn_set_stream_user_data: {}", nghttp3_strerror(rv));
         return false;
      }
      if (auto rv = nghttp3_conn_submit_response(h3, id, nv, nva.size(), &dr); rv != 0)
      {
         mloge("nghttp3_conn_submit_response: {}", nghttp3_strerror(rv));
         return false;
      }
   }

   headers_submitted = true;
   log_headers(log_prefix_, nva);
   return true;
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void Http3Stream::reset_writing()
{
   //
   // An HTTP/3 message cut short is an exchange that failed: both directions go, as with HTTP/2's
   // RST_STREAM.
   //
   session.reset_stream(id, reset_error_code());
   closed = true;
}

void Http3Stream::delete_writer()
{
   //
   // The teardown paths below can run handlers that drop the last reference to this stream,
   // erasing it from the session -- keep it alive until this function returns.
   //
   auto self = shared_from_this();

   //
   // Nothing to finalize on a stream ngtcp2 has already torn down (peer reset it, or we did):
   // there is nothing left to reset, and submitting anything would leave nghttp3 holding data for
   // a stream that no longer exists, which it would then offer for sending forever.
   //
   if (closed)
   {
      mlogd("delete_writer: stream already closed");
      maybe_close();
      return;
   }

   if (!headers_submitted)
   {
      //
      // The handler never even started its message (a server handler that returned, or whose
      // request was reset, before calling response.async_submit()). There is no HEADERS frame for
      // nghttp3 to close out, so ending the body has nothing to act on and the peer would be left
      // waiting forever. Abort the stream at the transport level instead, mirroring what
      // h3_cb_stop_sending/h3_cb_reset_stream already do for nghttp3-initiated aborts. NO_ERROR
      // here (rather than e.g. INTERNAL_ERROR): choosing not to respond isn't itself a protocol
      // error -- the peer just needs to be told the stream is over so it doesn't wait forever.
      //
      mlogd("delete_writer: no headers were ever submitted, shutting stream down");
      if (auto* conn = session.conn())
         ngtcp2_conn_shutdown_stream(conn, 0, id, NGHTTP3_H3_NO_ERROR);
      closed = true;
      session.wake_write();
      maybe_close();
      return;
   }

   if (!eof_submitted)
   {
      //
      // The Request/Response was dropped without ever ending the body (async_write({})), so
      // wherever it stopped is not where it was meant to stop. Sending a FIN here would present
      // that partial message to the peer as a complete one -- reset the stream instead, the way
      // the HTTP/2 side submits RST_STREAM once its writer is gone with no EOF submitted, and fail
      // the local read the same way nghttp2's stream close does, with partial_message.
      //
      mlogw("delete_writer: body never ended, resetting stream");
      session.reset_stream(id, reset_error_code());
      fail(errors::partial_message);
   }

   maybe_close();
}

// =================================================================================================

} // namespace anyhttp::http3
