//
// QuicSession: one QUIC connection, shared by the server and the client and by whatever protocol
// runs on top. See anyhttp/quic/session.hpp; the role-specific ends live in quic/server.cpp and
// quic/client.cpp, the protocols in their QuicApplication.
//
#include "anyhttp/quic/session.hpp"
#include "anyhttp/client_impl.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
#include "anyhttp/quic/common.hpp"
#include "anyhttp/quic/stream.hpp"
#include "anyhttp/tls.hpp"

#include <boost/system/detail/errc.hpp>

#include <openssl/rand.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <format> // IWYU pragma: keep
#include <stdexcept>

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::quic
{

// =================================================================================================

QuicSession::QuicSession(const Executor& executor) // NOLINT(performance-unnecessary-value-param)
   : executor_(executor), timer_(executor_), tx_buf_(64_k)
{
   ngtcp2_ccerr_default(&last_error_);
}

QuicSession::~QuicSession()
{
   timer_.cancel();
   clear_streams();
   application_.reset(); // before the connection it may still refer to
   if (conn_)
      ngtcp2_conn_del(conn_);
   if (ssl_)
   {
      SSL_set_app_data(ssl_, nullptr);
      SSL_free(ssl_);
   }
#if !ANYHTTP_QUIC_BORINGSSL
   if (ossl_ctx_)
      ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
#endif
}

void QuicSession::clear_streams() { streams_.clear(); }

void QuicSession::fail_streams(error_code ec)
{
   //
   // Streams may erase themselves from streams_ as a side effect of fail() (via maybe_close()),
   // so snapshot first.
   //
   std::vector<std::shared_ptr<QuicStream>> streams;
   streams.reserve(streams_.size());
   for (auto& [id, stream] : streams_)
      streams.push_back(stream);
   for (auto& stream : streams)
      stream->fail(ec);
}

// -------------------------------------------------------------------------------------------------

std::shared_ptr<QuicStream> QuicSession::find_stream(int64_t id)
{
   auto it = streams_.find(id);
   return it == streams_.end() ? nullptr : it->second;
}

QuicStream* QuicSession::create_stream(int64_t id)
{
   assert(application_);
   auto [it, inserted] = streams_.emplace(id, application_->make_stream(id));
   if (inserted)
      ++streams_created_;
   return it->second.get();
}

void QuicSession::erase_stream(int64_t id) { streams_.erase(id); }

// -------------------------------------------------------------------------------------------------

void QuicSession::consume_stream(int64_t stream_id, size_t n)
{
   if (n == 0 || !conn_)
      return;
   ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, n);
   wake_write(); // a WINDOW_UPDATE-equivalent frame needs to go out
}

void QuicSession::reset_stream(int64_t stream_id, uint64_t app_error_code)
{
   if (!conn_)
      return;
   ngtcp2_conn_shutdown_stream(conn_, 0, stream_id, app_error_code);
   wake_write();
}

void QuicSession::stop_reading(int64_t stream_id, uint64_t app_error_code)
{
   if (!conn_)
      return;
   ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
   wake_write();
}

void QuicSession::set_application_error(uint64_t app_error_code)
{
   ngtcp2_ccerr_set_application_error(&last_error_, app_error_code, nullptr, 0);
}

// -------------------------------------------------------------------------------------------------

void QuicSession::wake_write()
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

   run_later(get_executor(), [self = weak_from_this()] {
      auto session = std::static_pointer_cast<QuicSession>(self.lock());
      if (!session)
         return;
      session->write_posted_ = false;
      if (session->closed_)
         return;
      session->flush_write();
   });
}

int QuicSession::flush_write()
{
   if (closed_ || !conn_)
      return 0;

   if (auto rv = write_streams(); rv != 0)
      return rv;

   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

ngtcp2_ssize QuicSession::write_pkt_cb(ngtcp2_conn*, ngtcp2_path* path, ngtcp2_pkt_info* pi,
                                       uint8_t* dest, size_t destlen, ngtcp2_tstamp ts,
                                       void* user_data)
{
   return static_cast<QuicSession*>(user_data)->write_pkt(path, pi, dest, destlen, ts);
}

//
// Writes a single QUIC packet's worth of stream data into [dest, dest+destlen[. Called repeatedly
// by ngtcp2_conn_write_aggregate_pkt2() (once per packet it wants to pack into the shared TX
// buffer), so this must never send anything itself -- write_streams() decides when and how the
// accumulated packets go out.
//
ngtcp2_ssize QuicSession::write_pkt(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                    size_t destlen, ngtcp2_tstamp ts)
{
   std::array<ngtcp2_vec, 16> vec;
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
      bool fin = false;
      ngtcp2_ssize sveccnt = 0;

      if (application_ready_ && ngtcp2_conn_get_max_data_left(conn_))
      {
         sveccnt = application_->next_stream_data(stream_id, fin, vec);
         if (sveccnt < 0)
            return NGTCP2_ERR_CALLBACK_FAILURE; // the application has set last_error_
      }

      ngtcp2_ssize ndatalen;
      uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
      if (fin)
         flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

      auto nwrite =
         ngtcp2_conn_writev_stream(conn_, path, pi, dest, destlen, &ndatalen, flags, stream_id,
                                   vec.data(), static_cast<size_t>(sveccnt), ts);

      if (nwrite < 0)
      {
         switch (nwrite)
         {
         case NGTCP2_ERR_STREAM_DATA_BLOCKED:
            if (stream_id >= 0)
               application_->on_stream_blocked(stream_id);
            continue;
         case NGTCP2_ERR_STREAM_SHUT_WR:
            if (stream_id >= 0)
               application_->on_stream_shut_wr(stream_id);
            continue;
         case NGTCP2_ERR_STREAM_NOT_FOUND:
            //
            // ngtcp2 has already torn the stream down (the peer reset it, or we did) while the
            // application still had data queued for it. That's a dead stream, not a dead
            // connection -- tell the application so it stops offering it and keep serving the
            // others. Should it offer the same stream again anyway, stop packing this packet
            // rather than spinning here forever.
            //
            if (stream_id >= 0 && stream_id != shut_down_stream)
            {
               mlogw("write_pkt: stream {} is gone, shutting down its write side", stream_id);
               application_->on_stream_shut_wr(stream_id);
               application_->on_stream_blocked(stream_id);
               shut_down_stream = stream_id;
               continue;
            }
            return 0;
         case NGTCP2_ERR_WRITE_MORE:
            if (stream_id >= 0 && ndatalen > 0 &&
                application_->on_stream_data_written(stream_id, static_cast<size_t>(ndatalen)) != 0)
               return NGTCP2_ERR_CALLBACK_FAILURE;
            continue;
         default:
            mloge("ngtcp2_conn_writev_stream: {}", ngtcp2_strerror(static_cast<int>(nwrite)));
            ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      if (ndatalen > 0 && stream_id >= 0 &&
          application_->on_stream_data_written(stream_id, static_cast<size_t>(ndatalen)) != 0)
         return NGTCP2_ERR_CALLBACK_FAILURE;

      return nwrite;
   }
}

// https://nghttp2.org/ngtcp2/programmers-guide.html#pseudo-code-for-writing-packets-with-gso
int QuicSession::write_streams()
{
   if (!conn_)
      return 0;
   if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_))
      return 0;

   mlogd("write_streams: max_data_left={}", ngtcp2_conn_get_max_data_left(conn_));

   ngtcp2_path_storage ps;
   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   size_t gso_size = 0;
   auto nwrite =
      ngtcp2_conn_write_aggregate_pkt2(conn_, &ps.path, &pi, tx_buf_.data(), tx_buf_.size(),
                                       &gso_size, &write_pkt_cb, 0, timestamp());
   if (nwrite < 0)
   {
      mloge("ngtcp2_conn_write_aggregate_pkt2: {}", ngtcp2_strerror(static_cast<int>(nwrite)));
      if (!last_error_.error_code)
         ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
      return handle_error(static_cast<int>(nwrite));
   }

   if (nwrite == 0)
      return 0;

   auto data = std::span<const uint8_t>{tx_buf_.data(), static_cast<size_t>(nwrite)};
   return send_datagrams(ps.path, data, gso_size ? gso_size : data.size());
}

// -------------------------------------------------------------------------------------------------

void QuicSession::update_timer() { arm_timer_from_ngtcp2(); }

void QuicSession::arm_timer_from_ngtcp2()
{
   if (closed_ || !conn_)
      return;

   auto expiry = ngtcp2_conn_get_expiry2(conn_);
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

   auto now = timestamp();
   auto delay =
      expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};

   timer_.arm(delay, [self = weak_from_this()] {
      if (auto session = std::static_pointer_cast<QuicSession>(self.lock()))
         session->handle_expiry();
   });
}

int QuicSession::handle_expiry()
{
   auto now = timestamp();
   if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
   {
      //
      // NGTCP2_ERR_IDLE_CLOSE is how a connection whose peer simply stopped talking ends -- an
      // interrupted client leaves one behind per connection it had open -- so it is a normal end
      // of life, not a failure worth a warning. handle_error() takes it from here; what makes it
      // special is that the connection is then discarded silently, see there.
      //
      if (rv == NGTCP2_ERR_IDLE_CLOSE)
         mlogi("idle timeout, dropping connection");
      else
         mlogw("ngtcp2_conn_handle_expiry: {}", ngtcp2_strerror(rv));

      ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }
   return flush_write();
}

// -------------------------------------------------------------------------------------------------

int QuicSession::on_read(const ngtcp2_path& path, const ngtcp2_pkt_info& pi,
                         std::span<const uint8_t> data)
{
   mlogd("on_read: {} bytes", data.size());

   auto rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data.data(), data.size(), timestamp());
   if (rv != 0)
   {
      if (rv == NGTCP2_ERR_DRAINING)
         mlogd("ngtcp2_conn_read_pkt: draining");
      else
         mlogw("ngtcp2_conn_read_pkt: {}", ngtcp2_strerror(rv));

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

std::span<const uint8_t> QuicSession::write_connection_close(std::span<uint8_t> buf,
                                                             ngtcp2_path_storage& ps)
{
   if (!conn_)
      return {};

   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   auto nwrite = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, buf.data(), buf.size(),
                                                    &last_error_, timestamp());
   if (nwrite <= 0)
      return {};
   return buf.first(static_cast<size_t>(nwrite));
}

// =================================================================================================
// Connection setup
// =================================================================================================

void QuicSession::fill_callbacks(ngtcp2_callbacks& callbacks)
{
   callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
   callbacks.handshake_completed = &QuicSession::cb_handshake_completed;
   callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
   callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
   callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
   callbacks.recv_stream_data = &QuicSession::cb_recv_stream_data;
   callbacks.acked_stream_data_offset = &QuicSession::cb_acked_stream_data_offset;
   callbacks.stream_open = &QuicSession::cb_stream_open;
   callbacks.stream_close = &QuicSession::cb_stream_close;
   callbacks.rand = &QuicSession::cb_rand;
   callbacks.get_new_connection_id = &QuicSession::cb_get_new_connection_id;
   callbacks.remove_connection_id = &QuicSession::cb_remove_connection_id;
   callbacks.update_key = ngtcp2_crypto_update_key_cb;
   callbacks.stream_stop_sending = &QuicSession::cb_stream_stop_sending;
   callbacks.stream_reset = &QuicSession::cb_stream_reset;
   callbacks.extend_max_stream_data = &QuicSession::cb_extend_max_stream_data;
   callbacks.extend_max_local_streams_bidi = &QuicSession::cb_extend_max_streams_bidi;
   callbacks.extend_max_remote_streams_bidi = &QuicSession::cb_extend_max_streams_bidi;
   callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
   callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
   callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
   callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
   callbacks.recv_rx_key = &QuicSession::cb_recv_rx_key;
}

void QuicSession::fill_settings(ngtcp2_settings& settings, ngtcp2_transport_params& params,
                                std::chrono::nanoseconds idle_timeout)
{
   ngtcp2_settings_default(&settings);
   settings.initial_ts = timestamp();
   if (spdlog::default_logger_raw()->should_log(spdlog::level::trace))
      settings.log_printf = &quic::ngtcp2_log_printf;

   ngtcp2_transport_params_default(&params);
   params.initial_max_stream_data_bidi_local = 256_k;
   params.initial_max_stream_data_bidi_remote = 256_k;
   params.initial_max_stream_data_uni = 256_k;
   params.initial_max_data = 1_m;
   params.initial_max_streams_bidi = 100;
   params.initial_max_streams_uni = 3;
   params.max_idle_timeout = static_cast<uint64_t>(idle_timeout.count());
}

void QuicSession::configure_tls_context(SSL_CTX* ctx, bool is_server)
{
#if ANYHTTP_QUIC_BORINGSSL
   auto configure = is_server ? &ngtcp2_crypto_boringssl_configure_server_context
                              : &ngtcp2_crypto_boringssl_configure_client_context;
   if (configure(ctx) != 0)
      throw std::runtime_error(std::format("ngtcp2_crypto_boringssl_configure_{}_context failed",
                                           is_server ? "server" : "client"));

   //
   // What OpenSSL needs SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS, SSL_OP_SINGLE_ECDH_USE and
   // SSL_MODE_RELEASE_BUFFERS for is the default in BoringSSL. SSL_OP_NO_ANTI_REPLAY does not
   // exist, but it would only matter for 0-RTT, which we don't enable.
   //
   if (is_server)
      SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
#else
   //
   // Optional, but ngtcp2 recommends it: fetches the ciphers once instead of on every use.
   //
   static const int init_rv = ngtcp2_crypto_ossl_init();
   if (init_rv != 0)
      throw std::runtime_error("ngtcp2_crypto_ossl_init failed");

   if (is_server)
   {
      SSL_CTX_set_options(ctx, (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                                  SSL_OP_SINGLE_ECDH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE |
                                  SSL_OP_NO_ANTI_REPLAY);
      SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
   }
#endif
}

//
// With BoringSSL, the QUIC specifics were configured on the SSL_CTX already (see
// configure_tls_context()), so the session is a plain SSL whose app data leads back to us. With
// OpenSSL, ngtcp2 configures each SSL and wraps it into an ngtcp2_crypto_ossl_ctx.
//
int QuicSession::setup_tls(SSL_CTX* ssl_ctx, bool is_server)
{
   ssl_ = SSL_new(ssl_ctx);
   if (!ssl_)
   {
      mloge("SSL_new failed");
      return -1;
   }

   conn_ref_.get_conn = &QuicSession::get_conn;
   conn_ref_.user_data = this;
   SSL_set_app_data(ssl_, &conn_ref_);

   if (is_server)
      SSL_set_accept_state(ssl_);
   else
      SSL_set_connect_state(ssl_);

#if ANYHTTP_QUIC_BORINGSSL
   ngtcp2_conn_set_tls_native_handle(conn_, ssl_);
#else
   auto configure = is_server ? &ngtcp2_crypto_ossl_configure_server_session
                              : &ngtcp2_crypto_ossl_configure_client_session;
   if (configure(ssl_) != 0)
   {
      mloge("ngtcp2_crypto_ossl_configure_{}_session failed", is_server ? "server" : "client");
      return -1;
   }

   if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, ssl_) != 0)
   {
      mloge("ngtcp2_crypto_ossl_ctx_new failed");
      return -1;
   }

   ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);
#endif
   return 0;
}

QuicSession* QuicSession::from_ssl(SSL* ssl)
{
   auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(SSL_get_app_data(ssl));
   return ref ? static_cast<QuicSession*>(ref->user_data) : nullptr;
}

void QuicSession::set_application(std::unique_ptr<QuicApplication> application)
{
   assert(!application_);
   application_ = std::move(application);
   if (!conn_)
      return; // the client: what it fills in goes to ngtcp2_conn_client_new()

   ngtcp2_transport_params params = *ngtcp2_conn_get_local_transport_params(conn_);
   application_->fill_transport_params(params);
   if (auto rv = ngtcp2_conn_set_local_transport_params(conn_, &params); rv != 0)
      mloge("ngtcp2_conn_set_local_transport_params: {}", ngtcp2_strerror(rv));
}

int QuicSession::start_application()
{
   if (application_ready_)
      return 0;

   if (!application_)
   {
      mloge("no application protocol negotiated");
      return -1;
   }

   if (application_->on_ready() != 0)
      return -1;

   application_ready_ = true;
   on_application_ready();
   return 0;
}

// =================================================================================================

void QuicApplication::async_submit(SubmitHandler&& handler, std::string_view, boost::urls::url,
                                   const Fields&)
{
   std::move(handler)(make_error_code(errc::operation_not_supported), client::Request{nullptr});
}

// =================================================================================================
// ngtcp2 callback implementations
// =================================================================================================

//
// The one line per connection, like the TCP server's "new connection" (see handle_connection()):
// logged once the handshake has told what it negotiated. A connection that fails before is logged
// by whatever ends it instead.
//
int QuicSession::cb_handshake_completed(ngtcp2_conn* conn, void* user)
{
   auto self = static_cast<QuicSession*>(user);

   const unsigned char* alpn = nullptr;
   unsigned int alpnlen = 0;
   SSL_get0_alpn_selected(self->ssl_, &alpn, &alpnlen);

   logi("[{}] {}, {} over QUIC (version=0x{:x}, scid={}, {})", self->log_prefix_,
        ngtcp2_conn_is_server(conn) ? "new connection" : "connected",
        std::string_view{reinterpret_cast<const char*>(alpn), alpnlen},
        ngtcp2_conn_get_negotiated_version(conn), format_hex(self->scid_.data, self->scid_.datalen),
        tls_handshake_info(self->ssl_));
   if (self->start_application() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int QuicSession::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                     uint64_t offset, const uint8_t* data, size_t datalen,
                                     void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   const bool fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;
   logd("[{}] cb_recv_stream_data: stream={} offset={} datalen={} fin={} ready={}",
        self->log_prefix_, stream_id, offset, datalen, fin, self->application_ready_);
   if (!self->application_ready_)
   {
      logw("[{}] cb_recv_stream_data: DROPPING {} bytes on stream {} (application not ready)",
           self->log_prefix_, datalen, stream_id);
      return 0;
   }
   return self->application_->recv_stream_data(stream_id, {data, datalen}, fin);
}

int QuicSession::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset,
                                             uint64_t datalen, void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   if (!self->application_ready_)
      return 0;
   return self->application_->acked_stream_data(stream_id, offset, datalen);
}

int QuicSession::cb_stream_open(ngtcp2_conn*, int64_t /*stream_id*/, void* /*user*/) { return 0; }

int QuicSession::cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                 uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   if (!self->application_ready_)
      return 0;
   return self->application_->stream_close(stream_id, flags, app_error_code);
}

void QuicSession::cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
      std::memset(dest, 0, destlen);
}

int QuicSession::cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token,
                                          size_t cidlen, void* user)
{
   auto self = static_cast<QuicSession*>(user);
   if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   cid->datalen = cidlen;
   if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   self->on_new_cid(*cid);
   return 0;
}

int QuicSession::cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user)
{
   static_cast<QuicSession*>(user)->on_remove_cid(*cid);
   return 0;
}

int QuicSession::cb_extend_max_streams_bidi(ngtcp2_conn*, uint64_t /*max_streams*/, void* /*user*/)
{
   return 0;
}

int QuicSession::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                        void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   if (!self->application_ready_)
      return 0;
   return self->application_->stop_sending(stream_id, app_error_code);
}

int QuicSession::cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t /*final_size*/,
                                 uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   if (!self->application_ready_)
      return 0;
   return self->application_->stream_reset(stream_id, app_error_code);
}

int QuicSession::cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t /*max_data*/,
                                           void* user, void*)
{
   auto self = static_cast<QuicSession*>(user);
   if (!self->application_ready_)
      return 0;
   return self->application_->extend_max_stream_data(stream_id);
}

int QuicSession::cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user)
{
   if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
      return 0;
   auto self = static_cast<QuicSession*>(user);
   if (self->start_application() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

// =================================================================================================

} // namespace anyhttp::quic
