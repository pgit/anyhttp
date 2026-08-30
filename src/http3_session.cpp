//
// Http3Session: one QUIC connection carrying HTTP/3, shared by the server and the client.
// See anyhttp/http3_session.hpp; the role-specific ends live in server_impl_udp.cpp and
// client_impl_udp.cpp.
//
#include "anyhttp/http3_session.hpp"
#include "anyhttp/http3_stream.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/tls.hpp"

#include <boost/asio/post.hpp>
#include <boost/system/detail/errc.hpp>

#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstring>

#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::http3
{

// =================================================================================================

Http3Session::Http3Session(asio::any_io_executor executor)
   : executor_(std::move(executor)), timer_(executor_), tx_buf_(64_k)
{
   ngtcp2_ccerr_default(&last_error_);
}

Http3Session::~Http3Session()
{
   timer_.cancel();
   clear_streams();
   if (h3_)
      nghttp3_conn_del(h3_);
   if (conn_)
      ngtcp2_conn_del(conn_);
   if (ossl_ctx_)
   {
      if (auto ssl = ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_))
      {
         SSL_set_app_data(ssl, nullptr);
         SSL_free(ssl);
      }
      ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
   }
}

void Http3Session::clear_streams() { streams_.clear(); }

// -------------------------------------------------------------------------------------------------

std::shared_ptr<Http3Stream> Http3Session::find_stream(int64_t id)
{
   auto it = streams_.find(id);
   return it == streams_.end() ? nullptr : it->second;
}

Http3Stream* Http3Session::create_stream(int64_t id)
{
   auto [it, inserted] = streams_.emplace(id, make_stream(id));
   return it->second.get();
}

void Http3Session::erase_stream(int64_t id) { streams_.erase(id); }

// -------------------------------------------------------------------------------------------------

void Http3Session::consume_stream(int64_t stream_id, size_t n)
{
   if (n == 0 || !conn_)
      return;
   ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, n);
   wake_write(); // a WINDOW_UPDATE-equivalent frame needs to go out
}

void Http3Session::reset_stream(int64_t stream_id, uint64_t app_error_code)
{
   if (!conn_)
      return;
   ngtcp2_conn_shutdown_stream(conn_, 0, stream_id, app_error_code);
   wake_write();
}

void Http3Session::stop_reading(int64_t stream_id, uint64_t app_error_code)
{
   if (!conn_)
      return;
   ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
   wake_write();
}

// -------------------------------------------------------------------------------------------------

void Http3Session::wake_write()
{
   //
   // The write loop is normally only run in reaction to a packet arriving or a timer firing. When
   // the application submits data outside those events, we need to kick it ourselves.
   //
   // Capture a weak_ptr, not shared_from_this(): wake_write() can be reached from a Reader/Writer
   // destructor that runs as part of *this* session's own teardown (e.g. a still-in-flight
   // request/response destroyed when everything is cancelled on shutdown), at which point
   // shared_from_this() would throw bad_weak_ptr.
   //
   // One flush per wake, not one per submission. A response submits its headers, its body and its
   // EOF separately, and posting for each means the first pass writes everything and the rest
   // walk the connection for nothing -- and still re-arm the timer on the way out. Arming once
   // and clearing when the flush runs is what ngtcp2's example server gets for free from
   // ev_io_start() on an already-active watcher.
   //
   if (write_posted_)
      return;
   write_posted_ = true;

   asio::post(get_executor(), [self = weak_from_this()]
   {
      auto session = std::static_pointer_cast<Http3Session>(self.lock());
      if (!session)
         return;
      session->write_posted_ = false;
      if (session->closed_)
         return;
      session->flush_write();
   });
}

int Http3Session::flush_write()
{
   if (closed_ || !conn_)
      return 0;

   if (auto rv = write_streams(); rv != 0)
      return rv;

   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

ngtcp2_ssize Http3Session::write_pkt_cb(ngtcp2_conn*, ngtcp2_path* path, ngtcp2_pkt_info* pi,
                                        uint8_t* dest, size_t destlen, ngtcp2_tstamp ts,
                                        void* user_data)
{
   return static_cast<Http3Session*>(user_data)->write_pkt(path, pi, dest, destlen, ts);
}

//
// Writes a single QUIC packet's worth of stream data into [dest, dest+destlen). Called repeatedly
// by ngtcp2_conn_write_aggregate_pkt2() (once per packet it wants to pack into the shared TX
// buffer), so this must never send anything itself -- write_streams() decides when and how the
// accumulated packets go out.
//
ngtcp2_ssize Http3Session::write_pkt(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                     size_t destlen, ngtcp2_tstamp ts)
{
   std::array<nghttp3_vec, 16> vec;
   int64_t shut_down_stream = -1; // see NGTCP2_ERR_STREAM_NOT_FOUND below

   for (;;)
   {
      //
      // Everything below runs inside ngtcp2_conn_write_aggregate_pkt2(), which hands us the one
      // timestamp of this whole write pass and calls back once per packet it wants to pack. Should
      // the connection have been closed in between -- a handler resumed from a stream close or a
      // deferred consume can get there -- stop packing rather than feeding ngtcp2 a `ts` that is
      // now in its past, which it asserts on.
      //
      if (closed_ || !conn_)
         return 0;

      int64_t stream_id = -1;
      int fin = 0;
      nghttp3_ssize sveccnt = 0;

      if (h3_ && ngtcp2_conn_get_max_data_left(conn_))
      {
         sveccnt = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec.data(), vec.size());
         logd("[{}] write_pkt: nghttp3_conn_writev_stream -> stream={} sveccnt={} fin={}",
              log_prefix_, stream_id, sveccnt, fin);
         if (sveccnt < 0)
         {
            loge("[{}] nghttp3_conn_writev_stream: {}", log_prefix_,
                 nghttp3_strerror(static_cast<int>(sveccnt)));
            ngtcp2_ccerr_set_application_error(
               &last_error_, nghttp3_err_infer_quic_app_error_code(static_cast<int>(sveccnt)),
               nullptr, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      ngtcp2_ssize ndatalen;
      uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
      if (fin)
         flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

      auto nwrite = ngtcp2_conn_writev_stream(
         conn_, path, pi, dest, destlen, &ndatalen, flags, stream_id,
         reinterpret_cast<const ngtcp2_vec*>(vec.data()), static_cast<size_t>(sveccnt), ts);

      if (nwrite < 0)
      {
         switch (nwrite)
         {
         case NGTCP2_ERR_STREAM_DATA_BLOCKED:
            if (h3_ && stream_id >= 0)
               nghttp3_conn_block_stream(h3_, stream_id);
            continue;
         case NGTCP2_ERR_STREAM_SHUT_WR:
            if (h3_ && stream_id >= 0)
               nghttp3_conn_shutdown_stream_write(h3_, stream_id);
            continue;
         case NGTCP2_ERR_STREAM_NOT_FOUND:
            //
            // ngtcp2 has already torn the stream down (the peer reset it, or we did) while
            // nghttp3 still had data queued for it. That's a dead stream, not a dead connection
            // -- tell nghttp3 so it stops offering it and keep serving the others. Should nghttp3
            // offer the same stream again anyway, stop packing this packet rather than spinning
            // here forever.
            //
            if (h3_ && stream_id >= 0 && stream_id != shut_down_stream)
            {
               logw("[{}] write_pkt: stream {} is gone, shutting down its write side", log_prefix_,
                    stream_id);
               nghttp3_conn_shutdown_stream_write(h3_, stream_id);
               nghttp3_conn_block_stream(h3_, stream_id);
               shut_down_stream = stream_id;
               continue;
            }
            return 0;
         case NGTCP2_ERR_WRITE_MORE:
            if (h3_ && stream_id >= 0 && ndatalen > 0)
            {
               if (auto rv =
                      nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
                   rv != 0)
               {
                  loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
                  return NGTCP2_ERR_CALLBACK_FAILURE;
               }
               if (auto s = find_stream(stream_id))
                  s->on_write_offered(static_cast<size_t>(ndatalen));
            }
            continue;
         default:
            loge("[{}] ngtcp2_conn_writev_stream: {}", log_prefix_,
                 ngtcp2_strerror(static_cast<int>(nwrite)));
            ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      if (ndatalen > 0 && h3_ && stream_id >= 0)
      {
         if (auto rv = nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
             rv != 0)
         {
            loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
         if (auto s = find_stream(stream_id))
            s->on_write_offered(static_cast<size_t>(ndatalen));
      }

      return nwrite;
   }
}

int Http3Session::write_streams()
{
   if (!conn_)
      return 0;
   if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_))
      return 0;

   logd("[{}] write_streams: max_data_left={}", log_prefix_, ngtcp2_conn_get_max_data_left(conn_));

   ngtcp2_path_storage ps;
   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   size_t gso_size = 0;
   auto nwrite =
      ngtcp2_conn_write_aggregate_pkt2(conn_, &ps.path, &pi, tx_buf_.data(), tx_buf_.size(),
                                       &gso_size, &write_pkt_cb, 0, ngtcp2::util::timestamp());
   if (nwrite < 0)
   {
      loge("[{}] ngtcp2_conn_write_aggregate_pkt2: {}", log_prefix_,
           ngtcp2_strerror(static_cast<int>(nwrite)));
      if (!last_error_.error_code)
         ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
      return handle_error(static_cast<int>(nwrite));
   }

   ngtcp2_conn_update_pkt_tx_time(conn_, ngtcp2::util::timestamp());

   if (nwrite == 0)
      return 0;

   auto data = std::span<const uint8_t>{tx_buf_.data(), static_cast<size_t>(nwrite)};
   return send_datagrams(ps.path, data, gso_size ? gso_size : data.size());
}

// -------------------------------------------------------------------------------------------------

void Http3Session::update_timer() { arm_timer_from_ngtcp2(); }

void Http3Session::arm_timer_from_ngtcp2()
{
   if (closed_ || !conn_)
      return;

   auto expiry = ngtcp2_conn_get_expiry(conn_);
   if (expiry == UINT64_MAX)
   {
      //
      // ngtcp2 has no pending timer. Cancel the current one so we don't accidentally keep an old
      // retransmission timer alive past its purpose, and don't keep the io_context alive
      // indefinitely.
      //
      timer_.cancel();
      return;
   }

   auto now = ngtcp2::util::timestamp();
   asio::steady_timer::duration delay =
      expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};

   timer_.expires_after(delay);
   timer_.async_wait([self = weak_from_this()](const boost::system::error_code& ec)
   {
      if (ec)
         return;
      if (auto session = std::static_pointer_cast<Http3Session>(self.lock()))
         session->handle_expiry();
   });
}

int Http3Session::handle_expiry()
{
   auto now = ngtcp2::util::timestamp();
   if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
   {
      //
      // NGTCP2_ERR_IDLE_CLOSE is how a connection whose peer simply stopped talking ends -- an
      // interrupted client leaves one behind per connection it had open -- so it is a normal end
      // of life, not a failure worth a warning. handle_error() takes it from here; what makes it
      // special is that the connection is then discarded silently, see there.
      //
      if (rv == NGTCP2_ERR_IDLE_CLOSE)
         logi("[{}] idle timeout, dropping connection", log_prefix_);
      else
         logw("[{}] ngtcp2_conn_handle_expiry: {}", log_prefix_, ngtcp2_strerror(rv));

      ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }
   return flush_write();
}

// -------------------------------------------------------------------------------------------------

int Http3Session::on_read(const ngtcp2_path& path, const ngtcp2_pkt_info& pi,
                          std::span<const uint8_t> data)
{
   logd("[{}] on_read: {} bytes", log_prefix_, data.size());

   auto rv =
      ngtcp2_conn_read_pkt(conn_, &path, &pi, data.data(), data.size(), ngtcp2::util::timestamp());
   if (rv != 0)
   {
      if (rv == NGTCP2_ERR_DRAINING)
         logd("[{}] ngtcp2_conn_read_pkt: draining", log_prefix_);
      else
         logw("[{}] ngtcp2_conn_read_pkt: {}", log_prefix_, ngtcp2_strerror(rv));

      if (rv == NGTCP2_ERR_CRYPTO && !last_error_.error_code)
         ngtcp2_ccerr_set_tls_alert(&last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
      else if (!last_error_.error_code)
         ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }

   //
   // Deliberately no write here: the caller flushes once it has fed us everything that arrived,
   // see flush_write().
   //
   return 0;
}

std::span<const uint8_t> Http3Session::write_connection_close(std::span<uint8_t> buf,
                                                              ngtcp2_path_storage& ps)
{
   if (!conn_)
      return {};

   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   auto nwrite = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, buf.data(), buf.size(),
                                                    &last_error_, ngtcp2::util::timestamp());
   if (nwrite <= 0)
      return {};
   return buf.first(static_cast<size_t>(nwrite));
}

// =================================================================================================
// Connection setup
// =================================================================================================

void Http3Session::fill_callbacks(ngtcp2_callbacks& callbacks)
{
   callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
   callbacks.handshake_completed = &Http3Session::cb_handshake_completed;
   callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
   callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
   callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
   callbacks.recv_stream_data = &Http3Session::cb_recv_stream_data;
   callbacks.acked_stream_data_offset = &Http3Session::cb_acked_stream_data_offset;
   callbacks.stream_open = &Http3Session::cb_stream_open;
   callbacks.stream_close = &Http3Session::cb_stream_close;
   callbacks.rand = &Http3Session::cb_rand;
   callbacks.get_new_connection_id = &Http3Session::cb_get_new_connection_id;
   callbacks.remove_connection_id = &Http3Session::cb_remove_connection_id;
   callbacks.update_key = ngtcp2_crypto_update_key_cb;
   callbacks.stream_stop_sending = &Http3Session::cb_stream_stop_sending;
   callbacks.stream_reset = &Http3Session::cb_stream_reset;
   callbacks.extend_max_stream_data = &Http3Session::cb_extend_max_stream_data;
   callbacks.extend_max_local_streams_bidi = &Http3Session::cb_extend_max_streams_bidi;
   callbacks.extend_max_remote_streams_bidi = &Http3Session::cb_extend_max_streams_bidi;
   callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
   callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
   callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
   callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
   callbacks.recv_rx_key = &Http3Session::cb_recv_rx_key;
}

void Http3Session::fill_settings(ngtcp2_settings& settings, ngtcp2_transport_params& params,
                                 std::chrono::nanoseconds idle_timeout)
{
   ngtcp2_settings_default(&settings);
   settings.initial_ts = ngtcp2::util::timestamp();
   if (spdlog::default_logger_raw()->should_log(spdlog::level::trace))
      settings.log_printf = &http3::ngtcp2_log_printf;

   ngtcp2_transport_params_default(&params);
   params.initial_max_stream_data_bidi_local = 256_k;
   params.initial_max_stream_data_bidi_remote = 256_k;
   params.initial_max_stream_data_uni = 256_k;
   params.initial_max_data = 1_m;
   params.initial_max_streams_bidi = 100;
   params.initial_max_streams_uni = 3;
   params.max_idle_timeout = static_cast<uint64_t>(idle_timeout.count());
}

int Http3Session::setup_tls(SSL_CTX* ssl_ctx, bool is_server)
{
   auto* ssl = SSL_new(ssl_ctx);
   if (!ssl)
   {
      loge("[{}] SSL_new failed", log_prefix_);
      return -1;
   }

   conn_ref_.get_conn = &Http3Session::get_conn;
   conn_ref_.user_data = this;
   SSL_set_app_data(ssl, &conn_ref_);

   if (is_server)
      SSL_set_accept_state(ssl);
   else
      SSL_set_connect_state(ssl);

   auto configure = is_server ? &ngtcp2_crypto_ossl_configure_server_session
                              : &ngtcp2_crypto_ossl_configure_client_session;
   if (configure(ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_configure_{}_session failed", log_prefix_,
           is_server ? "server" : "client");
      SSL_free(ssl);
      return -1;
   }

   if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_ctx_new failed", log_prefix_);
      SSL_free(ssl);
      return -1;
   }

   ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);
   return 0;
}

int Http3Session::setup_http3()
{
   if (h3_)
      return 0;

   const bool is_server = ngtcp2_conn_is_server(conn_) != 0;

   nghttp3_callbacks h3cb{};
   h3cb.acked_stream_data = &Http3Session::h3_cb_acked_stream_data;
   h3cb.stream_close = &Http3Session::h3_cb_stream_close;
   h3cb.recv_data = &Http3Session::h3_cb_recv_data;
   h3cb.deferred_consume = &Http3Session::h3_cb_deferred_consume;
   h3cb.begin_headers = &Http3Session::h3_cb_begin_headers;
   h3cb.recv_header = &Http3Session::h3_cb_recv_header;
   h3cb.end_headers = &Http3Session::h3_cb_end_headers;
   h3cb.end_stream = &Http3Session::h3_cb_end_stream;
   h3cb.stop_sending = &Http3Session::h3_cb_stop_sending;
   h3cb.reset_stream = &Http3Session::h3_cb_reset_stream;

   nghttp3_settings settings;
   nghttp3_settings_default(&settings);
   settings.qpack_max_dtable_capacity = 4096;
   settings.qpack_blocked_streams = 100;

   if (is_server)
   {
      if (auto rv = nghttp3_conn_server_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
      {
         loge("[{}] nghttp3_conn_server_new: {}", log_prefix_, nghttp3_strerror(rv));
         return -1;
      }
      auto params = ngtcp2_conn_get_local_transport_params(conn_);
      nghttp3_conn_set_max_client_streams_bidi(h3_, params->initial_max_streams_bidi);
   }
   else if (auto rv = nghttp3_conn_client_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
   {
      loge("[{}] nghttp3_conn_client_new: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   int64_t ctrl_stream_id = -1;
   if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr); rv != 0)
   {
      loge("[{}] open control stream: {}", log_prefix_, ngtcp2_strerror(rv));
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_control_stream(h3_, ctrl_stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_bind_control_stream: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   int64_t qpack_enc_stream_id = -1;
   int64_t qpack_dec_stream_id = -1;
   if (ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr) != 0 ||
       ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr) != 0)
   {
      loge("[{}] open qpack streams failed", log_prefix_);
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_qpack_streams(h3_, qpack_enc_stream_id, qpack_dec_stream_id);
       rv != 0)
   {
      loge("[{}] nghttp3_conn_bind_qpack_streams: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   logi("[{}] HTTP/3 ready (ctrl={} qpack_enc={} qpack_dec={})", log_prefix_, ctrl_stream_id,
        qpack_enc_stream_id, qpack_dec_stream_id);

   on_http3_ready();
   return 0;
}

// =================================================================================================
// ngtcp2 callback implementations
// =================================================================================================

int Http3Session::cb_handshake_completed(ngtcp2_conn*, void* user)
{
   auto self = static_cast<Http3Session*>(user);
   logi("[{}] TLS handshake completed: {}", self->log_prefix_,
        tls_handshake_info(ngtcp2_crypto_ossl_ctx_get_ssl(self->ossl_ctx_)));
   if (self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int Http3Session::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                      uint64_t offset, const uint8_t* data, size_t datalen,
                                      void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   logd("[{}] cb_recv_stream_data: stream={} offset={} datalen={} fin={} h3_={}", self->log_prefix_,
        stream_id, offset, datalen, !!(flags & NGTCP2_STREAM_DATA_FLAG_FIN), !!self->h3_);
   if (!self->h3_)
   {
      logw("[{}] cb_recv_stream_data: DROPPING {} bytes on stream {} (h3 not ready)",
           self->log_prefix_, datalen, stream_id);
      return 0;
   }

   auto nread = nghttp3_conn_read_stream(self->h3_, stream_id, data, datalen,
                                         (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0);
   if (nread < 0)
   {
      loge("[{}] nghttp3_conn_read_stream({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(static_cast<int>(nread)));
      ngtcp2_ccerr_set_application_error(
         &self->last_error_, nghttp3_err_infer_quic_app_error_code(static_cast<int>(nread)),
         nullptr, 0);
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }

   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, static_cast<uint64_t>(nread));
   ngtcp2_conn_extend_max_offset(self->conn_, static_cast<uint64_t>(nread));
   return 0;
}

int Http3Session::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t /*offset*/,
                                              uint64_t datalen, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_add_ack_offset(self->h3_, stream_id, datalen); rv != 0)
   {
      loge("[{}] nghttp3_conn_add_ack_offset: {}", self->log_prefix_, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Session::cb_stream_open(ngtcp2_conn*, int64_t /*stream_id*/, void* /*user*/) { return 0; }

int Http3Session::cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                  uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
      app_error_code = NGHTTP3_H3_NO_ERROR;
   if (self->h3_)
   {
      if (auto rv = nghttp3_conn_close_stream(self->h3_, stream_id, app_error_code); rv != 0)
      {
         if (rv == NGHTTP3_ERR_STREAM_NOT_FOUND)
            return 0;
         loge("[{}] nghttp3_conn_close_stream({}): {}", self->log_prefix_, stream_id,
              nghttp3_strerror(rv));
         return NGTCP2_ERR_CALLBACK_FAILURE;
      }
   }
   return 0;
}

void Http3Session::cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
      std::memset(dest, 0, destlen);
}

int Http3Session::cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token,
                                           size_t cidlen, void* user)
{
   auto self = static_cast<Http3Session*>(user);
   if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   cid->datalen = cidlen;
   if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   self->on_new_cid(*cid);
   return 0;
}

int Http3Session::cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user)
{
   static_cast<Http3Session*>(user)->on_remove_cid(*cid);
   return 0;
}

int Http3Session::cb_extend_max_streams_bidi(ngtcp2_conn*, uint64_t /*max_streams*/, void* /*user*/)
{
   return 0;
}

int Http3Session::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t /*ec*/,
                                         void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_shutdown_stream_read(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_shutdown_stream_read({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Session::cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t /*final_size*/,
                                  uint64_t /*ec*/, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_shutdown_stream_read(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_shutdown_stream_read({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Session::cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t /*max_data*/,
                                            void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_unblock_stream(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_unblock_stream({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Session::cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user)
{
   if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
      return 0;
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_ && self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

// =================================================================================================
// nghttp3 callback implementations
// =================================================================================================

//
// The only notification that the peer is done with body bytes we handed out by reference, and
// hence that the caller's buffer may be released -- see WriteMode::ZeroCopy.
//
int Http3Session::h3_cb_acked_stream_data(nghttp3_conn*, int64_t stream_id, uint64_t datalen,
                                          void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   auto stream = self->find_stream(stream_id);
   if (!stream)
      return 0;

   //
   // Completing a write resumes the application, which may drop the last reference to this
   // session -- while ngtcp2 is still in the middle of processing the ACK that got us here.
   // weak_from_this(), not shared_from_this(): the ACK may well arrive during teardown.
   //
   auto session_guard = self->weak_from_this().lock();
   stream->on_write_acked(static_cast<size_t>(datalen));
   return 0;
}

int Http3Session::h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   logd("[{}] h3 stream {} closed", self->log_prefix_, stream_id);
   if (auto s = self->find_stream(stream_id))
   {
      //
      // Anything still pending on this stream must be completed now: nothing will ever arrive for
      // it again, and a stream that ended in an error has, by definition, not delivered its whole
      // message.
      //
      auto ec = (app_error_code == NGHTTP3_H3_NO_ERROR)
                   ? boost::system::error_code{}
                   : boost::system::errc::make_error_code(boost::system::errc::connection_reset);
      s->fail(ec);
   }
   if (ngtcp2_conn_is_server(self->conn_))
      ngtcp2_conn_extend_max_streams_bidi(self->conn_, 1);
   return 0;
}

int Http3Session::h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data,
                                  size_t datalen, void* user, void*)
{
   //
   // Connection-level credit is granted immediately: it is a single pool shared with control/QPACK
   // streams that nghttp3 manages on its own (the app never "reads" those), so withholding it here
   // would stall unrelated traffic whenever this one stream's reader is slow. Only the *stream*-
   // level credit for these bytes is deliberately deferred -- see Http3Session::consume_stream().
   // Granting it only once the application actually reads the data (in
   // Http3Stream::call_read_handler()) is what makes body backpressure real instead of nghttp3
   // buffering an unbounded backlog in pending_read while the peer keeps sending on *this* stream.
   //
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_extend_max_offset(self->conn_, datalen);
   if (auto s = self->find_stream(stream_id))
      s->on_data_chunk(data, datalen);
   return 0;
}

int Http3Session::h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                         void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, nconsumed);
   ngtcp2_conn_extend_max_offset(self->conn_, nconsumed);
   return 0;
}

int Http3Session::h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   //
   // On the server this is where a stream comes into being: the peer opened it. On the client the
   // stream was created by async_submit() long before its response arrives, so this finds it.
   //
   auto self = static_cast<Http3Session*>(user);
   if (!self->find_stream(stream_id))
      self->create_stream(stream_id);
   return 0;
}

int Http3Session::h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t /*token*/,
                                    nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t /*flags*/,
                                    void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   auto n = nghttp3_rcbuf_get_buf(name);
   auto v = nghttp3_rcbuf_get_buf(value);

   if (auto s = self->find_stream(stream_id))
      s->on_header(std::string_view{reinterpret_cast<const char*>(n.base), n.len},
                   std::string_view{reinterpret_cast<const char*>(v.base), v.len});
   return 0;
}

int Http3Session::h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user,
                                    void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_end_headers();
   return 0;
}

int Http3Session::h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_eof();
   return 0;
}

int Http3Session::h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_shutdown_stream_read(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

int Http3Session::h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_shutdown_stream_write(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

// =================================================================================================

} // namespace anyhttp::http3
