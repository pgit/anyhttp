//
// Http3Application: HTTP/3 on a QUIC connection, shared by the server and the client. See
// anyhttp/h3/application.hpp; the role-specific ends live in h3/server.cpp and h3/client.cpp.
//
#include "anyhttp/h3/application.hpp"
#include "anyhttp/h3/stream.hpp"
#include "anyhttp/quic/session.hpp"
#include "anyhttp/tls.hpp"

#include <boost/system/detail/errc.hpp>

#include <format> // IWYU pragma: keep

namespace anyhttp::http3
{

// =================================================================================================

Http3Application::Http3Application(quic::QuicSession& session, size_t max_header_size)
   : QuicApplication(session), max_header_size_(max_header_size)
{
}

Http3Application::~Http3Application()
{
   if (h3_)
      nghttp3_conn_del(h3_);
}

const std::string& Http3Application::log_prefix() const noexcept { return session_.log_prefix(); }

std::shared_ptr<Http3Stream> Http3Application::find_stream(int64_t id)
{
   // every stream of this session was made by make_stream() of an Http3Application
   return std::static_pointer_cast<Http3Stream>(session_.find_stream(id));
}

// -------------------------------------------------------------------------------------------------

void Http3Application::fill_transport_params(ngtcp2_transport_params& params)
{
   // room for the peer's control and QPACK encoder/decoder streams
   params.initial_max_streams_uni = 3;
}

int Http3Application::on_ready()
{
   if (h3_)
      return 0;

   auto* conn = session_.conn();
   const bool is_server = ngtcp2_conn_is_server(conn) != 0;

   nghttp3_callbacks h3cb{};
   h3cb.acked_stream_data = &Http3Application::h3_cb_acked_stream_data;
   h3cb.stream_close = &Http3Application::h3_cb_stream_close;
   h3cb.recv_data = &Http3Application::h3_cb_recv_data;
   h3cb.deferred_consume = &Http3Application::h3_cb_deferred_consume;
   h3cb.begin_headers = &Http3Application::h3_cb_begin_headers;
   h3cb.recv_header = &Http3Application::h3_cb_recv_header;
   h3cb.end_headers = &Http3Application::h3_cb_end_headers;
   h3cb.end_stream = &Http3Application::h3_cb_end_stream;
   h3cb.stop_sending = &Http3Application::h3_cb_stop_sending;
   h3cb.reset_stream = &Http3Application::h3_cb_reset_stream;
   //
   // Optional, unlike ngtcp2's and nghttp2 version 2's: without it nghttp3 keeps its pre-1.11
   // behavior of unrandomized seeds, which upstream recommends against as a hardening measure.
   //
   h3cb.rand = &tls_random_bytes;

   nghttp3_settings settings;
   nghttp3_settings_default(&settings);
   settings.qpack_max_dtable_capacity = 4096;
   settings.qpack_blocked_streams = 100;

   //
   // Only announced to the peer: nghttp3 does not enforce it, see Http3Stream::on_header(). The
   // setting goes out either way, as nghttp3 always writes it into its SETTINGS frame. Leaving it
   // at the default would announce an unlimited header section, and take four more bytes to do so.
   // * https://datatracker.ietf.org/doc/html/rfc9114#section-4.2.2
   //
   settings.max_field_section_size = max_header_size_;

   if (is_server)
   {
      if (auto rv = nghttp3_conn_server_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
      {
         mloge("nghttp3_conn_server_new: {}", nghttp3_strerror(rv));
         return -1;
      }
      auto params = ngtcp2_conn_get_local_transport_params(conn);
      nghttp3_conn_set_max_client_streams_bidi(h3_, params->initial_max_streams_bidi);
   }
   else if (auto rv = nghttp3_conn_client_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
   {
      mloge("nghttp3_conn_client_new: {}", nghttp3_strerror(rv));
      return -1;
   }

   int64_t ctrl_stream_id = -1;
   if (auto rv = ngtcp2_conn_open_uni_stream(conn, &ctrl_stream_id, nullptr); rv != 0)
   {
      mloge("open control stream: {}", ngtcp2_strerror(rv));
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_control_stream(h3_, ctrl_stream_id); rv != 0)
   {
      mloge("nghttp3_conn_bind_control_stream: {}", nghttp3_strerror(rv));
      return -1;
   }

   int64_t qpack_enc_stream_id = -1;
   int64_t qpack_dec_stream_id = -1;
   if (ngtcp2_conn_open_uni_stream(conn, &qpack_enc_stream_id, nullptr) != 0 ||
       ngtcp2_conn_open_uni_stream(conn, &qpack_dec_stream_id, nullptr) != 0)
   {
      mloge("open qpack streams failed");
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_qpack_streams(h3_, qpack_enc_stream_id, qpack_dec_stream_id);
       rv != 0)
   {
      mloge("nghttp3_conn_bind_qpack_streams: {}", nghttp3_strerror(rv));
      return -1;
   }

   mlogd("HTTP/3 ready (ctrl={} qpack_enc={} qpack_dec={})", ctrl_stream_id, qpack_enc_stream_id,
         qpack_dec_stream_id);
   return 0;
}

// =================================================================================================
// From ngtcp2, through the session
// =================================================================================================

int Http3Application::recv_stream_data(int64_t stream_id, std::span<const uint8_t> data, bool fin)
{
   auto nread = nghttp3_conn_read_stream(h3_, stream_id, data.data(), data.size(), fin ? 1 : 0);
   if (nread < 0)
   {
      mloge("nghttp3_conn_read_stream({}): {}", stream_id,
            nghttp3_strerror(static_cast<int>(nread)));
      session_.set_application_error(
         nghttp3_err_infer_quic_app_error_code(static_cast<int>(nread)));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }

   //
   // What nghttp3 consumed itself -- framing, headers, the control and QPACK streams -- is credit
   // the peer gets back right away. Body bytes are left out of nread: their credit comes with
   // h3_cb_recv_data() (connection) and Http3Stream::call_read_handler() (stream).
   //
   auto* conn = session_.conn();
   ngtcp2_conn_extend_max_stream_offset(conn, stream_id, static_cast<uint64_t>(nread));
   ngtcp2_conn_extend_max_offset(conn, static_cast<uint64_t>(nread));
   return 0;
}

int Http3Application::acked_stream_data(int64_t stream_id, uint64_t /*offset*/, uint64_t len)
{
   if (auto rv = nghttp3_conn_add_ack_offset(h3_, stream_id, len); rv != 0)
   {
      mloge("nghttp3_conn_add_ack_offset: {}", nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Application::stream_close(int64_t stream_id, uint32_t flags, uint64_t app_error_code)
{
   if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
      app_error_code = NGHTTP3_H3_NO_ERROR;
   if (auto rv = nghttp3_conn_close_stream(h3_, stream_id, app_error_code); rv != 0)
   {
      if (rv == NGHTTP3_ERR_STREAM_NOT_FOUND)
         return 0;
      mloge("nghttp3_conn_close_stream({}): {}", stream_id, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Application::stream_reset(int64_t stream_id, uint64_t /*app_error_code*/)
{
   if (auto rv = nghttp3_conn_shutdown_stream_read(h3_, stream_id); rv != 0)
   {
      mloge("nghttp3_conn_shutdown_stream_read({}): {}", stream_id, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Application::stream_stop_sending(int64_t stream_id, uint64_t /*app_error_code*/)
{
   if (auto rv = nghttp3_conn_shutdown_stream_read(h3_, stream_id); rv != 0)
   {
      mloge("nghttp3_conn_shutdown_stream_read({}): {}", stream_id, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Application::extend_max_stream_data(int64_t stream_id)
{
   if (auto rv = nghttp3_conn_unblock_stream(h3_, stream_id); rv != 0)
   {
      mloge("nghttp3_conn_unblock_stream({}): {}", stream_id, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

// -------------------------------------------------------------------------------------------------

//
// nghttp3 is the scheduler: it picks the stream, and frames what goes out on it.
//
ngtcp2_ssize Http3Application::next_stream_data(int64_t& stream_id, bool& fin,
                                                std::span<ngtcp2_vec> vec)
{
   int pfin = 0;
   static_assert(sizeof(nghttp3_vec) == sizeof(ngtcp2_vec));
   auto sveccnt = nghttp3_conn_writev_stream(
      h3_, &stream_id, &pfin, reinterpret_cast<nghttp3_vec*>(vec.data()), vec.size());
   mlogd("write_pkt: nghttp3_conn_writev_stream -> stream={} sveccnt={} fin={}", stream_id, sveccnt,
         pfin);
   if (sveccnt < 0)
   {
      mloge("nghttp3_conn_writev_stream: {}", nghttp3_strerror(static_cast<int>(sveccnt)));
      session_.set_application_error(
         nghttp3_err_infer_quic_app_error_code(static_cast<int>(sveccnt)));
      return sveccnt;
   }
   fin = pfin != 0;
   return sveccnt;
}

int Http3Application::on_stream_data_written(int64_t stream_id, size_t n, bool /*fin*/)
{
   //
   // nghttp3 keeps track of its FIN itself, and has never been told about a packet that carries
   // nothing of a stream but the FIN.
   //
   if (n == 0)
      return 0;
   if (auto rv = nghttp3_conn_add_write_offset(h3_, stream_id, n); rv != 0)
   {
      mloge("nghttp3_conn_add_write_offset: {}", nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   if (auto s = find_stream(stream_id))
      s->on_write_offered(n);
   return 0;
}

void Http3Application::on_stream_blocked(int64_t stream_id)
{
   nghttp3_conn_block_stream(h3_, stream_id);
}

void Http3Application::on_stream_shut_wr(int64_t stream_id)
{
   nghttp3_conn_shutdown_stream_write(h3_, stream_id);
}

// =================================================================================================
// nghttp3 callback implementations
// =================================================================================================

//
// The only notification that the peer is done with body bytes we handed out by reference, and
// hence that the caller's buffer may be released -- see WriteMode::ZeroCopy.
//
int Http3Application::h3_cb_acked_stream_data(nghttp3_conn*, int64_t stream_id, uint64_t datalen,
                                              void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   auto stream = self->find_stream(stream_id);
   if (!stream)
      return 0;

   //
   // Completing a write resumes the application, which may drop the last reference to this
   // session -- while ngtcp2 is still in the middle of processing the ACK that got us here.
   // weak_from_this(), not shared_from_this(): the ACK may well arrive during teardown.
   //
   auto session_guard = self->session_.weak_from_this().lock();
   stream->on_write_acked(static_cast<size_t>(datalen));
   return 0;
}

int Http3Application::h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                         void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   logd("[{}] h3 stream {} closed", self->log_prefix(), stream_id);
   if (auto s = self->find_stream(stream_id))
   {
      //
      // Anything still pending on this stream must be completed now: nothing will ever arrive for
      // it again, and a stream that ended in an error has, by definition, not delivered its whole
      // message.
      //
      auto ec = (app_error_code == NGHTTP3_H3_NO_ERROR) ? error_code{}
                                                        : make_error_code(errc::connection_reset);
      s->fail(ec);
   }
   auto* conn = self->session_.conn();
   if (ngtcp2_conn_is_server(conn))
      ngtcp2_conn_extend_max_streams_bidi(conn, 1);
   return 0;
}

int Http3Application::h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data,
                                      size_t datalen, void* user, void*)
{
   //
   // Connection-level credit is granted immediately: it is a single pool shared with control/QPACK
   // streams that nghttp3 manages on its own (the app never "reads" those), so withholding it here
   // would stall unrelated traffic whenever this one stream's reader is slow. Only the *stream*-
   // level credit for these bytes is deliberately deferred -- see QuicSession::consume_stream().
   // Granting it only once the application actually reads the data (in
   // Http3Stream::call_read_handler()) is what makes body backpressure real instead of nghttp3
   // buffering an unbounded backlog in pending_read while the peer keeps sending on *this* stream.
   //
   auto self = static_cast<Http3Application*>(user);
   ngtcp2_conn_extend_max_offset(self->session_.conn(), datalen);
   if (auto s = self->find_stream(stream_id))
      s->on_data_chunk(data, datalen);
   return 0;
}

int Http3Application::h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                             void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   auto* conn = self->session_.conn();
   ngtcp2_conn_extend_max_stream_offset(conn, stream_id, nconsumed);
   ngtcp2_conn_extend_max_offset(conn, nconsumed);
   return 0;
}

int Http3Application::h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   //
   // On the server this is where a stream comes into being: the peer opened it. On the client the
   // stream was created by async_submit() long before its response arrives, so this finds it.
   //
   auto self = static_cast<Http3Application*>(user);
   if (!self->session_.find_stream(stream_id))
      self->session_.create_stream(stream_id);
   return 0;
}

int Http3Application::h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t /*token*/,
                                        nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                                        uint8_t /*flags*/, void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   auto n = nghttp3_rcbuf_get_buf(name);
   auto v = nghttp3_rcbuf_get_buf(value);

   if (auto s = self->find_stream(stream_id))
      s->on_header(std::string_view{reinterpret_cast<const char*>(n.base), n.len},
                   std::string_view{reinterpret_cast<const char*>(v.base), v.len});
   return 0;
}

int Http3Application::h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user,
                                        void*)
{
   auto self = static_cast<Http3Application*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_end_headers();
   return 0;
}

int Http3Application::h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_eof();
   return 0;
}

int Http3Application::h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                         void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   ngtcp2_conn_shutdown_stream_read(self->session_.conn(), 0, stream_id, app_error_code);
   return 0;
}

int Http3Application::h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                         void* user, void*)
{
   auto self = static_cast<Http3Application*>(user);
   ngtcp2_conn_shutdown_stream_write(self->session_.conn(), 0, stream_id, app_error_code);
   return 0;
}

// =================================================================================================

} // namespace anyhttp::http3
