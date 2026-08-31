//
// Http3Stream: one HTTP/3 request/response exchange, shared by the server and the client.
// See anyhttp/h3_stream.hpp for the model; the role-specific ends of it live in
// h3_server.cpp and h3_client.cpp.
//
#include "anyhttp/h3_stream.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3_session.hpp"

#include <boost/asio/post.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/detail/errc.hpp>

#include <charconv>

using namespace boost::asio;
namespace errc = boost::system::errc;

namespace anyhttp::http3
{

// =================================================================================================

Http3Stream::Http3Stream(Http3Session& s, int64_t stream_id, WriteMode mode)
   : id(stream_id), session(s), write_mode(mode)
{
   log_prefix = std::format("{}.{}", session.logPrefix(), id);
   mlogd("\x1b[1;33mStream: ctor\x1b[0m");
}

Http3Stream::~Http3Stream()
{
   mlogd("\x1b[33mStream: dtor... \x1b[0m");
   //
   // A Http3Writer/Http3Reader (owned by the user-visible Request/Response) can outlive this
   // stream, e.g. when the session tears down its streams while a suspended coroutine still holds
   // one. Detach them so their destructors don't dereference a freed stream.
   //
   if (reader)
      reader->detach();
   if (writer)
      writer->detach();
   if (read_handler)
      swap_and_invoke(read_handler, errc::make_error_code(errc::connection_reset), 0);
   if (write_active && write_handler)
      swap_and_invoke(write_handler, errc::make_error_code(errc::connection_reset));
   mlogd("\x1b[33mStream: dtor... done\x1b[0m");
}

asio::any_io_executor Http3Stream::get_executor() const noexcept { return session.get_executor(); }

// =================================================================================================
// Incoming body
// =================================================================================================

void Http3Stream::on_data_chunk(const uint8_t* data, size_t len)
{
   if (len == 0)
      return;

   //
   // nghttp3 hands us a view into the packet it is parsing, valid only until this callback
   // returns. Offer it to a waiting reader as it stands before copying it anywhere: a handler
   // that keeps a read outstanding -- the usual shape -- takes the bytes with a single copy, and
   // the vector that would otherwise carry them (a malloc, a copy in, a copy out and a free, per
   // QUIC packet, so about fifty of each per 64k of body) is never created at all. Only what the
   // reader could not take is parked for later.
   //
   auto self = shared_from_this(); // a resumed reader may drop the last reference to this stream
   incoming = asio::const_buffer{data, len};
   call_read_handler();

   if (incoming.size() > 0)
   {
      auto* rest = static_cast<const uint8_t*>(incoming.data());
      pending_read.emplace_back(rest, rest + incoming.size());
      if (read_head.size() == 0)
         read_head = asio::buffer(pending_read.front());
      incoming = {};
   }
}

void Http3Stream::on_eof()
{
   eof_received = true;
   call_read_handler();
}

void Http3Stream::call_read_handler()
{
   //
   // swap_and_invoke() below may resume a user coroutine that calls async_read_some() again
   // before returning, which re-enters this function. Letting that nested call do real work would
   // recurse once per buffered chunk -- with enough data queued up (e.g. after a large backlog
   // drains), that blows the C++ stack. Instead, the nested call just re-arms read_handler and
   // returns; the outer call's loop below picks it up and keeps going without growing the stack.
   //
   if (!read_handler || call_read_handler_active)
      return;

   //
   // The loop below may resume a coroutine that drops the last owning reference to this stream
   // (e.g. the Response gets destroyed once EOF is delivered) -- or to the whole Session, when
   // that coroutine was the last user of the connection. Keep both alive until this function
   // returns: consume_stream() at the bottom dereferences the session, so outliving the stream
   // alone is not enough.
   //
   auto self = shared_from_this();
   auto session_guard = session.shared_from_this();

   call_read_handler_active = true;
   size_t consumed = 0;
   while (read_handler)
   {
      if (read_head.size() > 0 || incoming.size() > 0)
      {
         //
         // Fill the caller's buffer from as many queued chunks as it takes, rather than stopping
         // at the end of the first one. Each chunk is what arrived in a single QUIC packet -- a
         // little over a kilobyte -- so handing them out one per read turns a 64k body into ~48
         // reads, and a handler that answers every read with a write (an echo) pays a full round
         // trip for each of them when the write only completes on acknowledgement (see
         // WriteMode::ZeroCopy).
         //
         auto dest = read_handler_buffer;
         size_t copied = 0;
         while (dest.size() > 0 && read_head.size() > 0)
         {
            auto n = asio::buffer_copy(dest, read_head);
            dest += n;
            read_head += n;
            copied += n;
            if (read_head.size() == 0)
            {
               pending_read.pop_front();
               read_head =
                  pending_read.empty() ? asio::const_buffer{} : asio::buffer(pending_read.front());
            }
         }

         //
         // ... and last from the chunk being delivered right now, which on_data_chunk() offers
         // through `incoming` instead of parking it in a vector of its own first. Queued chunks
         // go first: they arrived earlier.
         //
         if (dest.size() > 0 && incoming.size() > 0)
         {
            auto n = asio::buffer_copy(dest, incoming);
            incoming += n;
            copied += n;
         }

         consumed += copied;
         swap_and_invoke(read_handler, boost::system::error_code{}, copied);
         continue;
      }

      if (eof_received)
      {
         // 0-byte read = EOF, matching the beast/nghttp2 convention.
         swap_and_invoke(read_handler, boost::system::error_code{}, 0);
         continue;
      }

      if (closed)
      {
         //
         // The stream died before the body was complete, and this read was issued after that --
         // there is nothing left that could ever complete it, so report the truncation now rather
         // than leaving it pending forever.
         //
         swap_and_invoke(read_handler, boost::beast::http::error::partial_message, 0);
         continue;
      }

      break;
   }
   call_read_handler_active = false;

   //
   // Grant the peer more send credit only for what was actually delivered to the app -- see
   // Http3Session::consume_stream() for why this must not happen any earlier.
   //
   session.consume_stream(id, consumed);
}

// =================================================================================================
// Outgoing body
// =================================================================================================

void Http3Stream::start_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   auto n = asio::buffer_size(buffer);
   const bool is_eof = (n == 0);
   logd("[{}] start_write: n={} is_eof={}", log_prefix, n, is_eof);

   //
   // Once accepted, the caller's intent to end the body is final: this is what tells
   // delete_writer() the body ended where it was meant to, so it need not reset the stream. An
   // earlier cancellation just makes for a shorter body than planned -- legitimate here, and a
   // declared content-length is still enforced by the peer.
   //
   if (is_eof && eof_submitted)
   {
      //
      // The body was already ended. If that FIN is still pending (its handler was detached by
      // cancellation, see bind_write_cancellation()), adopt this handler so it completes when the
      // FIN actually goes out; otherwise the FIN is long gone and there is nothing left to do.
      //
      if (write_active && write_is_eof)
      {
         logd("[{}] start_write: FIN already pending, adopting handler", log_prefix);
         bind_write_cancellation(handler, write_token);
         write_handler = std::move(handler);
      }
      else if (handler)
      {
         asio::any_completion_executor ex =
            asio::get_associated_immediate_executor(handler, get_executor());
         ex.execute([handler = std::move(handler)]() mutable
         { std::move(handler)(boost::system::error_code{}); });
      }
      return;
   }

   //
   // Only one async_write() may be active at a time -- see the class comment above write_active.
   // The re-issued EOF handled above is not an exception to that: it adopts the FIN that is
   // already in flight instead of starting a write of its own, and has returned by now.
   //
   assert(!write_active);

   if (is_eof)
      eof_submitted = true;

   const uint64_t token = next_write_token++;
   bind_write_cancellation(handler, token);

   write_active = true;
   write_source = buffer; // referenced, not copied -- see class comment above write_active
   write_offered = 0;
   write_acked = 0;
   write_source_copied = 0;
   write_chunk.clear();
   write_confirmed = 0;
   write_is_eof = is_eof;
   write_token = token;
   write_handler = std::move(handler);

   if (auto h3 = session.h3())
      nghttp3_conn_resume_stream(h3, id);
   session.wake_write();
}

void Http3Stream::bind_write_cancellation(WriteHandler& handler, uint64_t token)
{
   // Nothing to bind for a caller that passed no completion handler.
   if (!handler)
      return;

   auto cs = asio::get_associated_cancellation_slot(handler);
   if (!cs.is_connected() || cs.has_handler())
      return;

   cs.assign([this, token](asio::cancellation_type_t ct)
   {
      //
      // Cancellation completes the write immediately, without waiting for what it would normally
      // complete on -- see below for what that costs in either write mode.
      //
      if (write_token != token || !write_handler)
         return; // already completed naturally before the cancellation was delivered

      if (write_is_eof)
      {
         //
         // The body has already been declared ended, and a FIN cannot be un-sent -- it may just
         // still be waiting for flow control credit. Detach the handler but leave the write
         // active so it still goes out: abandoning it would leave the stream half-open forever,
         // with the peer waiting for an end that never comes.
         //
         logd("[{}] async_write: \x1b[1;31mcancelled\x1b[0m ({}), FIN still pending", log_prefix,
              ct);
         asio::post(get_executor(), [handler = std::move(write_handler)]() mutable { //
            std::move(handler)(errc::make_error_code(errc::operation_canceled));
         });
         return;
      }
      logd("[{}] async_write: \x1b[1;31mcancelled\x1b[0m ({})", log_prefix, ct);

      if (write_mode == WriteMode::ZeroCopy)
      {
         //
         // The handler runs now, and the caller is free to destroy its buffer the moment it does
         // -- but nghttp3/ngtcp2 point straight into that buffer (see WriteMode::ZeroCopy), so
         // whatever was offered and is not acknowledged yet has to stop being referenced first.
         // Only a reset can guarantee that: RESET_STREAM makes ngtcp2 drop the stream's queued
         // data and keeps it from reclaiming in-flight bytes for retransmission. That costs
         // nothing in expressiveness -- a body cut short mid-write is truncated, which is exactly
         // what delete_writer() resets the stream for as well.
         //
         // A write that never got to offer a byte, or whose bytes are all acknowledged already,
         // leaves nothing behind and lets the stream carry on unharmed.
         //
         if (write_offered > write_acked && !closed)
         {
            logw("[{}] async_write: cancelled with {} bytes unacknowledged, resetting stream",
                 log_prefix, write_offered - write_acked);
            session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
            closed = true;
         }
      }
      else
      {
         //
         // Staged mode never lets nghttp3 see the caller's buffer, only our own copy of it, so
         // the un-copied remainder can simply be abandoned -- same as HTTP/2, where cancelling
         // drops the unsent remainder of write_buffer. Bytes already offered still go out (they
         // can't be un-offered), so write_chunk is retired to in_flight_writes to keep that
         // memory alive. The stream stays healthy, and the caller may issue a fresh async_write()
         // as soon as the handler fires.
         //
         if (!write_chunk.empty())
            in_flight_writes.emplace_back(std::move(write_chunk));
         write_chunk.clear(); // moved-from
      }

      write_active = false;
      write_source = {};
      // make sure to post this -- otherwise "MAIN COROUTINE DID NOT COMPLETE" happens
      asio::post(get_executor(), [handler = std::move(write_handler)]() mutable { //
         std::move(handler)(errc::make_error_code(errc::operation_canceled));
      });
   });
}

nghttp3_ssize Http3Stream::data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags)
{
   if (veccnt == 0)
      return 0;

   if (!write_active)
      return NGHTTP3_ERR_WOULDBLOCK;

   if (write_mode == WriteMode::ZeroCopy)
   {
      //
      // Hand out what is left of the caller's buffer, by reference and in one go: a nghttp3_vec
      // is just a pointer and a length, so there is nothing to be gained from slicing it up, and
      // nghttp3 gets to frame the whole thing as a single DATA frame. It picks up the rest by
      // itself as packets are filled -- see Http3Session::write_pkt(), which keeps feeding the
      // same vec to ngtcp2_conn_writev_stream() and advances nghttp3 by whatever went into the
      // packet.
      //
      const size_t total = asio::buffer_size(write_source);
      if (write_offered < total)
      {
         auto* base = static_cast<const uint8_t*>(write_source.data()) + write_offered;
         vec[0].base = const_cast<uint8_t*>(base); // nghttp3 reads through this, never writes
         vec[0].len = total - write_offered;
         write_offered = total; // don't offer these bytes twice -- see class comment above
                                // write_active
         return 1;
      }
   }
   else
   {
      if (write_offered < write_chunk.size())
      {
         vec[0].base = write_chunk.data() + write_offered;
         vec[0].len = write_chunk.size() - write_offered;
         write_offered = write_chunk.size(); // don't re-offer these bytes on a repeat call
         return 1;
      }

      //
      // Current chunk fully offered. If it hasn't been confirmed yet (on_write_offered()), there
      // is nothing new until that happens -- carving off the next slice of write_source early
      // would have nghttp3 take the repeat offer as additional stream bytes.
      //
      if (write_confirmed < write_chunk.size())
         return NGHTTP3_ERR_WOULDBLOCK;

      //
      // The current chunk is fully drained; retire it (ngtcp2 may still need this exact memory
      // for retransmission until acked) and pull the next bounded slice out of write_source, if
      // any.
      //
      if (!write_chunk.empty())
         in_flight_writes.emplace_back(std::move(write_chunk));

      const size_t remaining = asio::buffer_size(write_source) - write_source_copied;
      if (remaining > 0)
      {
         const size_t take = std::min(remaining, kWriteChunkSize);
         auto* src = static_cast<const uint8_t*>(write_source.data()) + write_source_copied;
         write_chunk.assign(src, src + take);
         write_source_copied += take;
         write_offered = write_chunk.size();
         write_confirmed = 0;
         vec[0].base = write_chunk.data();
         vec[0].len = write_chunk.size();
         return 1;
      }
   }

   //
   // Everything has been offered. For a body write there is nothing new until the caller starts
   // the next one (which resumes the stream), so block here rather than returning 0 bytes --
   // returning 0 without NGHTTP3_DATA_FLAG_EOF would tell nghttp3 the body ended.
   //
   if (!write_is_eof)
      return NGHTTP3_ERR_WOULDBLOCK;

   //
   // The EOF marker (write_source is always empty for it) completes as soon as nghttp3 has taken
   // the FIN: unlike body data, a FIN carries no memory of the caller's that we would have to
   // keep alive until it is acknowledged.
   //
   *pflags |= NGHTTP3_DATA_FLAG_EOF;
   finish_active_write();
   return 0;
}

void Http3Stream::on_write_acked(size_t n)
{
   //
   // n counts *application* data acknowledged on this stream -- nghttp3 accounts for the HTTP/3
   // framing it puts around the body itself, so unlike ngtcp2's stream offsets these bytes are
   // exactly the ones the caller handed us. All of them belong to the write currently active: a
   // write only completes once every byte it offered is acknowledged, so nothing can still be
   // outstanding from an earlier one. Clamp defensively anyway -- an accounting mismatch should
   // complete the write early, not run write_acked past the end of the buffer.
   //
   if (write_mode != WriteMode::ZeroCopy)
      return; // a staged write is long done by the time its bytes are acknowledged
   if (n == 0 || !write_active || write_is_eof)
      return;

   write_acked = std::min(write_acked + n, asio::buffer_size(write_source));
   logd("[{}] on_write_acked: {} bytes, {}/{} acknowledged", log_prefix, n, write_acked,
        asio::buffer_size(write_source));

   if (write_acked == asio::buffer_size(write_source))
      finish_active_write();
}

void Http3Stream::on_write_offered(size_t n)
{
   //
   // n is the number of bytes of *stream* data ngtcp2 just committed to a packet, which also
   // includes the HTTP/3 HEADERS frame nghttp3 sends ahead of any body -- e.g. the very first
   // write pass after async_submit() drains the headers before there is an active write yet.
   // Only attribute bytes once there is an active, non-EOF write to charge them against; clamp
   // defensively in case a single packet still straddles the header/body boundary.
   //
   if (write_mode != WriteMode::Staged)
      return; // a zero-copy write completes on acknowledgement, not on handover
   if (n == 0 || !write_active || write_is_eof)
      return;

   n = std::min(n, write_chunk.size() - write_confirmed);
   write_confirmed += n;

   if (write_confirmed < write_chunk.size())
      return;

   //
   // The write is fully done once its current chunk is confirmed and there is no more of
   // write_source left to carve into further chunks -- data_reader() advances write_chunk/
   // write_source_copied otherwise, so this is the terminal state.
   //
   if (write_source_copied == asio::buffer_size(write_source))
   {
      finish_active_write();
      return;
   }

   //
   // There is more of write_source to carve into chunks, but nghttp3 may have asked for data
   // while this chunk was offered and still unconfirmed, in which case data_reader() answered
   // NGHTTP3_ERR_WOULDBLOCK -- and a blocked stream is never polled again until it is explicitly
   // resumed. Now that the chunk is confirmed, there is something new to hand out, so unblock the
   // stream. Without this, any single async_write() larger than kWriteChunkSize stalls here
   // forever, with the body truncated and no FIN.
   //
   if (auto h3 = session.h3())
      nghttp3_conn_resume_stream(h3, id);
   session.wake_write();
}

void Http3Stream::finish_active_write()
{
   assert(write_active);

   //
   // Invoking the handler hands the caller's buffer back to it, so this must only ever run when
   // nothing points into it any more: every offered byte acknowledged (ZeroCopy), copied out
   // (Staged), or no bytes offered at all (the EOF marker). A staged chunk stays alive past that
   // in in_flight_writes -- ngtcp2 may still retransmit from it.
   //
   if (write_mode == WriteMode::Staged && !write_chunk.empty())
      in_flight_writes.emplace_back(std::move(write_chunk));
   write_chunk.clear(); // moved-from
   write_source = {};
   auto handler = std::move(write_handler);
   write_active = false;

   if (!handler)
      return;

   //
   // Every path here runs inside a nghttp3 callback and hence inside an ngtcp2 call: the EOF
   // marker completes from data_reader() and a staged chunk from on_write_offered(), both while
   // ngtcp2 is packing a packet, and acknowledged data from acked_stream_data while it is parsing
   // one. Resuming the application there would let it call back into ngtcp2 from inside ngtcp2
   // (destroying the session writes a CONNECTION_CLOSE, say), which at best confuses the write
   // pass this is nested in and at worst trips ngtcp2's own "time must not go backwards"
   // assertion. Post instead -- one hop, on a path that is not latency critical.
   //
   asio::post(get_executor(), [self = shared_from_this(), handler = std::move(handler)]() mutable
   { swap_and_invoke(handler, boost::system::error_code{}); });
}

// =================================================================================================
// Headers
// =================================================================================================

void Http3Stream::on_header(std::string_view name, std::string_view value)
{
   if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))
      received_headers.emplace_back(name, value);

   try
   {
      if (name.starts_with(':'))
         on_pseudo_header(name, value);
      else if (name == "content-length")
      {
         size_t len = 0;
         if (std::from_chars(value.begin(), value.end(), len).ec == std::errc{})
            content_length = len;
      }
      else
         fields.set(name, value);
   }
   catch (const std::exception& ex)
   {
      logw("[{}] ignoring invalid header: {} ({})", log_prefix, value, ex.what());
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
   auto* h3 = session.h3();
   if (!h3)
   {
      loge("[{}] submit_headers: HTTP/3 layer is gone", log_prefix);
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
         loge("[{}] nghttp3_conn_submit_request: {}", log_prefix, nghttp3_strerror(rv));
         return false;
      }
   }
   else
   {
      if (auto rv = nghttp3_conn_set_stream_user_data(h3, id, this); rv != 0)
      {
         loge("[{}] nghttp3_conn_set_stream_user_data: {}", log_prefix, nghttp3_strerror(rv));
         return false;
      }
      if (auto rv = nghttp3_conn_submit_response(h3, id, nv, nva.size(), &dr); rv != 0)
      {
         loge("[{}] nghttp3_conn_submit_response: {}", log_prefix, nghttp3_strerror(rv));
         return false;
      }
   }

   headers_submitted = true;
   log_headers(log_prefix, nva);
   return true;
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void Http3Stream::fail(boost::system::error_code ec)
{
   //
   // The handlers below may run synchronously and drop the last owning reference to this stream
   // (e.g. the coroutine they resume destroys its Request/Response), reentrantly erasing it from
   // the session. Keep it alive until fail() itself returns.
   //
   auto self = shared_from_this();
   closed = true;

   if (read_handler)
   {
      //
      // The stream died before the body was complete. What the reader cares about is that it will
      // never see the rest of it, not which QUIC error code carried that news -- report the
      // truncation, matching what the HTTP/2 side delivers for a stream closing early.
      //
      auto read_ec = (ec && !eof_received) ? boost::beast::http::error::partial_message : ec;
      swap_and_invoke(read_handler, read_ec, 0);
   }

   on_failed(ec);

   //
   // A write waiting for its data to be acknowledged will never see those acknowledgements now:
   // ngtcp2 drops whatever of this stream is still in flight. It also stops touching the caller's
   // buffer, which is all the wait was ever for, so complete the write -- as failed, because the
   // body did not make it -- instead of leaving it pending forever.
   //
   if (write_active && write_handler)
   {
      write_active = false;
      write_source = {};
      swap_and_invoke(write_handler, ec ? ec : errc::make_error_code(errc::connection_reset));
   }

   maybe_close();
}

void Http3Stream::delete_reader()
{
   auto self = shared_from_this(); // see delete_writer()
   pending_read.clear();
   read_head = {};
   incoming = {};

   //
   // The application dropped its side of the exchange without reading the body to its end (e.g.
   // not_found(), which never looks at the request). Stream-level flow control credit is only
   // granted as the application actually reads (see Http3Session::consume_stream()), so a peer
   // with more body to send would stall forever against a window that will now never reopen.
   // Tell it to stop instead: STOP_SENDING half-closes only our read direction, leaving whatever
   // we are still writing to flow normally -- HTTP/2 has to submit a full RST_STREAM here for
   // lack of a half-close.
   //
   if (!eof_received && !closed)
   {
      logd("[{}] delete_reader: body not read to end, sending STOP_SENDING", log_prefix);
      session.stop_reading(id, NGHTTP3_H3_NO_ERROR);
   }

   maybe_close();
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
      logd("[{}] delete_writer: stream already closed", log_prefix);
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
      logd("[{}] delete_writer: no headers were ever submitted, shutting stream down", log_prefix);
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
      // the HTTP/2 side submits RST_STREAM once its writer is gone with no EOF submitted, and
      // fail the local read the same way nghttp2's stream close does, with partial_message.
      //
      logw("[{}] delete_writer: body never ended, resetting stream", log_prefix);
      session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
      fail(boost::beast::http::error::partial_message);
   }

   maybe_close();
}

void Http3Stream::maybe_close()
{
   if (reader || writer)
      return;
   if (!closed)
      return;
   session.erase_stream(id);
}

// =================================================================================================

} // namespace anyhttp::http3
