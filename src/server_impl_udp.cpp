//
// anyhttp QUIC / HTTP/3 server (first slice).
//
// This is intentionally minimal: TLS handshake + a hardcoded 200 OK response for
// any request. It is not yet wired into the anyhttp Session::Impl / RequestHandler
// abstractions -- follow-up commits will layer QUICSession on top of this.
//
// Supporting machinery (retry tokens, version negotiation, stateless reset,
// connection migration, GSO, ECN) is deliberately not implemented yet. Enough
// packet metadata handling remains to be interoperable with curl on the same box.
//

#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
#include "anyhttp/server_impl.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <netinet/udp.h>
#include <net/if.h>
#include <sys/socket.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ngtcp2/shared.h"
#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::server
{

// =================================================================================================
// Endpoint: a shim describing the local UDP socket the way ngtcp2 wants to see it.
// One process-wide socket, but ngtcp2 needs it wrapped in an Address+fd struct.
// =================================================================================================

struct Endpoint
{
   ngtcp2::Address addr;
   int fd;
};

// =================================================================================================
// Free-standing helpers
// =================================================================================================

namespace
{

constexpr size_t QUIC_SCIDLEN = 18;
constexpr std::string_view QUIC_ALPN_H3 = "\x2h3";

//
// One-shot process-wide initialization of ngtcp2_crypto_ossl and the OpenSSL SSL_CTX
// used for every QUIC connection. Lazily created on the first incoming packet.
//
struct TlsServerContext
{
   TlsServerContext()
   {
      static const int init_once = []
      {
         if (ngtcp2_crypto_ossl_init() != 0)
            throw std::runtime_error("ngtcp2_crypto_ossl_init");
         return 0;
      }();
      (void)init_once;

      ctx = SSL_CTX_new(TLS_server_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new");

      SSL_CTX_set_options(ctx,
                          (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                             SSL_OP_SINGLE_ECDH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE |
                             SSL_OP_NO_ANTI_REPLAY);
      SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);

      SSL_CTX_set_alpn_select_cb(ctx, &TlsServerContext::alpn_select_cb, nullptr);

      if (SSL_CTX_use_PrivateKey_file(ctx, "pki/out/server-key.pem", SSL_FILETYPE_PEM) != 1)
         throw std::runtime_error(std::string{"SSL_CTX_use_PrivateKey_file: "} +
                                  ERR_error_string(ERR_get_error(), nullptr));

      if (SSL_CTX_use_certificate_chain_file(ctx, "pki/out/server-chain.pem") != 1)
         throw std::runtime_error(std::string{"SSL_CTX_use_certificate_chain_file: "} +
                                  ERR_error_string(ERR_get_error(), nullptr));

      if (SSL_CTX_check_private_key(ctx) != 1)
         throw std::runtime_error("SSL_CTX_check_private_key");
   }

   ~TlsServerContext()
   {
      if (ctx)
         SSL_CTX_free(ctx);
   }

   TlsServerContext(const TlsServerContext&) = delete;
   TlsServerContext& operator=(const TlsServerContext&) = delete;

   static int alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                             const unsigned char* in, unsigned int inlen, void*)
   {
      // Look for "h3" in the client's ALPN list (length-prefixed strings).
      for (auto s = std::span{in, inlen}; s.size() >= 3; s = s.subspan(s[0] + 1))
      {
         if (s[0] == 2 && s[1] == 'h' && s[2] == '3')
         {
            *out = &s[1];
            *outlen = 2;
            return SSL_TLSEXT_ERR_OK;
         }
      }
      return SSL_TLSEXT_ERR_ALERT_FATAL;
   }

   SSL_CTX* ctx = nullptr;
};

TlsServerContext& tls_context()
{
   static TlsServerContext instance;
   return instance;
}

// -------------------------------------------------------------------------------------------------

std::string cid_key(const ngtcp2_cid& cid)
{
   return std::string{reinterpret_cast<const char*>(cid.data), cid.datalen};
}

std::string cid_key(const uint8_t* data, size_t len)
{
   return std::string{reinterpret_cast<const char*>(data), len};
}

// -------------------------------------------------------------------------------------------------

int send_udp(int fd, const sockaddr* sa, socklen_t salen, std::span<const uint8_t> data)
{
   for (;;)
   {
      auto n = ::sendto(fd, data.data(), data.size(), 0, sa, salen);
      if (n == -1)
      {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; // treat as best-effort; caller will retry on next tick
         loge("sendto: {}", strerror(errno));
         return -1;
      }
      return 0;
   }
}

} // namespace

// =================================================================================================
// Handler: one QUIC connection.
// Owns the ngtcp2_conn, its SSL, the nghttp3_conn, and the retransmission timer.
// =================================================================================================

class QuicHandler : public std::enable_shared_from_this<QuicHandler>
{
public:
   QuicHandler(Server::Impl& server, Endpoint ep, ngtcp2::Address remote);
   ~QuicHandler();

   // First entry point for a brand-new connection: create ngtcp2_conn + SSL and consume
   // the first (Initial) packet. `dcid` and `scid` come from the client packet header.
   int init(const ngtcp2_cid& dcid, const ngtcp2_cid& scid, uint32_t version,
            const ngtcp2_pkt_info& pi, std::span<const uint8_t> data);

   // Process a subsequent packet on an existing connection.
   int on_read(const ngtcp2_pkt_info& pi, std::span<const uint8_t> data,
               const ngtcp2::Address& remote);

   // Drain any pending ngtcp2/nghttp3 output.
   int write_streams();

   // Called after a read or timer tick to arm/rearm the retransmission timer.
   void update_timer();

   // Fired by the asio steady_timer.
   int handle_expiry();

   const ngtcp2_cid& scid() const noexcept { return scid_; }
   ngtcp2_conn* conn() const noexcept { return conn_; }
   bool closed() const noexcept { return closed_; }
   const std::string& log_prefix() const noexcept { return log_prefix_; }

   //
   // ngtcp2 <-> ngtcp2_crypto_ossl bridge: crypto lib calls back into ngtcp2 via this
   // ref that lives inside the SSL's app data.
   //
   static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref)
   {
      return static_cast<QuicHandler*>(ref->user_data)->conn_;
   }

   //
   // Callback bridges (static -> instance)
   //
   static int cb_handshake_completed(ngtcp2_conn*, void* user);
   static int cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t offset,
                                  const uint8_t* data, size_t datalen, void* user, void*);
   static int cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset,
                                          uint64_t datalen, void* user, void*);
   static int cb_stream_open(ngtcp2_conn*, int64_t stream_id, void* user);
   static int cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                              uint64_t app_error_code, void* user, void*);
   static void cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*);
   static int cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                       void* user);
   static int cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user);
   static int cb_extend_max_remote_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user);
   static int cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*);
   static int cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t final_size,
                              uint64_t app_error_code, void* user, void*);
   static int cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t max_data,
                                        void* user, void*);
   static int cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user);

   //
   // nghttp3 callback bridges
   //
   static int h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data, size_t datalen,
                              void* user, void*);
   static int h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                     void* user, void*);
   static int h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token,
                                nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t flags,
                                void* user, void*);
   static int h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int fin, void* user, void*);
   static int h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);

private:
   int setup_http3();
   int submit_response(int64_t stream_id);
   int handle_error(int rv);
   void arm_timer_from_ngtcp2();

private:
   Server::Impl& server_;
   Endpoint ep_;
   ngtcp2::Address remote_;
   ngtcp2_cid scid_{};

   ngtcp2_conn* conn_ = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr; // owns the SSL
   ngtcp2_crypto_conn_ref conn_ref_{};

   nghttp3_conn* h3_ = nullptr;

   asio::steady_timer timer_;
   ngtcp2_ccerr last_error_{};
   bool closed_ = false;

   std::string log_prefix_;

   // Cached to hand back to nghttp3's data reader for the hardcoded response.
   static constexpr std::string_view kResponseBody =
      "Hello from anyhttp QUIC!\n"
      "\n"
      "This is a hardcoded response served by the first-slice HTTP/3 server.\n";
};

// -------------------------------------------------------------------------------------------------

QuicHandler::QuicHandler(Server::Impl& server, Endpoint ep, ngtcp2::Address remote)
   : server_(server), ep_(ep), remote_(remote), timer_(server.get_executor())
{
   ngtcp2_ccerr_default(&last_error_);
   log_prefix_ = std::format("h3:{}", ngtcp2::util::straddr(&remote_.su.sa, remote_.len));
}

QuicHandler::~QuicHandler()
{
   timer_.cancel();
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
   logd("[{}] handler destroyed", log_prefix_);
}

// -------------------------------------------------------------------------------------------------

int QuicHandler::init(const ngtcp2_cid& dcid, const ngtcp2_cid& scid, uint32_t version,
                      const ngtcp2_pkt_info& pi, std::span<const uint8_t> data)
{
   // Our source CID: what we want the client to use to address us going forward.
   scid_.datalen = QUIC_SCIDLEN;
   if (RAND_bytes(scid_.data, static_cast<int>(scid_.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for SCID failed", log_prefix_);
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
   callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
   callbacks.handshake_completed = &QuicHandler::cb_handshake_completed;
   callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
   callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
   callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
   callbacks.recv_stream_data = &QuicHandler::cb_recv_stream_data;
   callbacks.acked_stream_data_offset = &QuicHandler::cb_acked_stream_data_offset;
   callbacks.stream_open = &QuicHandler::cb_stream_open;
   callbacks.stream_close = &QuicHandler::cb_stream_close;
   callbacks.rand = &QuicHandler::cb_rand;
   callbacks.get_new_connection_id = &QuicHandler::cb_get_new_connection_id;
   callbacks.remove_connection_id = &QuicHandler::cb_remove_connection_id;
   callbacks.update_key = ngtcp2_crypto_update_key_cb;
   callbacks.stream_reset = &QuicHandler::cb_stream_reset;
   callbacks.extend_max_remote_streams_bidi = &QuicHandler::cb_extend_max_remote_streams_bidi;
   callbacks.extend_max_stream_data = &QuicHandler::cb_extend_max_stream_data;
   callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
   callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
   callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
   callbacks.stream_stop_sending = &QuicHandler::cb_stream_stop_sending;
   callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
   callbacks.recv_rx_key = &QuicHandler::cb_recv_rx_key;

   ngtcp2_settings settings;
   ngtcp2_settings_default(&settings);
   settings.initial_ts = ngtcp2::util::timestamp();

   ngtcp2_transport_params params;
   ngtcp2_transport_params_default(&params);
   params.initial_max_stream_data_bidi_local = 256_k;
   params.initial_max_stream_data_bidi_remote = 256_k;
   params.initial_max_stream_data_uni = 256_k;
   params.initial_max_data = 1_m;
   params.initial_max_streams_bidi = 100;
   params.initial_max_streams_uni = 3;
   params.max_idle_timeout = std::chrono::nanoseconds(30s).count();
   params.original_dcid = dcid;
   params.original_dcid_present = 1;

   ngtcp2_path path{
      {const_cast<sockaddr*>(&ep_.addr.su.sa), ep_.addr.len},
      {const_cast<sockaddr*>(&remote_.su.sa), remote_.len},
      &ep_,
   };

   if (auto rv = ngtcp2_conn_server_new(&conn_, &scid, &scid_, &path, version, &callbacks,
                                        &settings, &params, nullptr, this);
       rv != 0)
   {
      loge("[{}] ngtcp2_conn_server_new: {}", log_prefix_, ngtcp2_strerror(rv));
      return -1;
   }

   auto* ssl = SSL_new(tls_context().ctx);
   if (!ssl)
   {
      loge("[{}] SSL_new failed", log_prefix_);
      return -1;
   }

   conn_ref_.get_conn = &QuicHandler::get_conn;
   conn_ref_.user_data = this;
   SSL_set_app_data(ssl, &conn_ref_);
   SSL_set_accept_state(ssl);

   if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_configure_server_session failed", log_prefix_);
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

   logi("[{}] new connection, scid={} version=0x{:x}", log_prefix_,
        ngtcp2::util::format_hex(scid_.data, scid_.datalen), version);

   return on_read(pi, data, remote_);
}

// -------------------------------------------------------------------------------------------------

int QuicHandler::on_read(const ngtcp2_pkt_info& pi, std::span<const uint8_t> data,
                         const ngtcp2::Address& remote)
{
   ngtcp2_path path{
      {const_cast<sockaddr*>(&ep_.addr.su.sa), ep_.addr.len},
      {const_cast<sockaddr*>(&remote.su.sa), remote.len},
      &ep_,
   };

   auto rv =
      ngtcp2_conn_read_pkt(conn_, &path, &pi, data.data(), data.size(), ngtcp2::util::timestamp());
   if (rv != 0)
   {
      // DRAINING is a normal client-initiated close; the rest is an actual failure.
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

   if (auto wrv = write_streams(); wrv != 0)
      return wrv;

   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

int QuicHandler::write_streams()
{
   if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_))
      return 0;

   std::array<uint8_t, 1500> buf;
   ngtcp2_path_storage ps;
   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   std::array<nghttp3_vec, 16> vec;

   for (;;)
   {
      int64_t stream_id = -1;
      int fin = 0;
      nghttp3_ssize sveccnt = 0;

      if (h3_ && ngtcp2_conn_get_max_data_left(conn_))
      {
         sveccnt = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec.data(), vec.size());
         if (sveccnt < 0)
         {
            loge("[{}] nghttp3_conn_writev_stream: {}", log_prefix_,
                 nghttp3_strerror(static_cast<int>(sveccnt)));
            ngtcp2_ccerr_set_application_error(
               &last_error_,
               nghttp3_err_infer_quic_app_error_code(static_cast<int>(sveccnt)), nullptr, 0);
            return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
         }
      }

      ngtcp2_ssize ndatalen;
      uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
      if (fin)
         flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

      auto nwrite = ngtcp2_conn_writev_stream(
         conn_, &ps.path, &pi, buf.data(), buf.size(), &ndatalen, flags, stream_id,
         reinterpret_cast<const ngtcp2_vec*>(vec.data()), static_cast<size_t>(sveccnt),
         ngtcp2::util::timestamp());

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
         case NGTCP2_ERR_WRITE_MORE:
            if (h3_ && stream_id >= 0 && ndatalen > 0)
            {
               if (auto rv = nghttp3_conn_add_write_offset(h3_, stream_id,
                                                           static_cast<size_t>(ndatalen));
                   rv != 0)
               {
                  loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_,
                       nghttp3_strerror(rv));
                  return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
               }
            }
            continue;
         default:
            loge("[{}] ngtcp2_conn_writev_stream: {}", log_prefix_,
                 ngtcp2_strerror(static_cast<int>(nwrite)));
            ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
            return handle_error(static_cast<int>(nwrite));
         }
      }

      if (ndatalen > 0 && h3_ && stream_id >= 0)
      {
         if (auto rv =
                nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
             rv != 0)
         {
            loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
            return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
         }
      }

      if (nwrite == 0)
      {
         ngtcp2_conn_update_pkt_tx_time(conn_, ngtcp2::util::timestamp());
         return 0;
      }

      if (send_udp(ep_.fd, ps.path.remote.addr, ps.path.remote.addrlen,
                   {buf.data(), static_cast<size_t>(nwrite)}) != 0)
      {
         return -1;
      }
   }
}

// -------------------------------------------------------------------------------------------------

void QuicHandler::update_timer() { arm_timer_from_ngtcp2(); }

void QuicHandler::arm_timer_from_ngtcp2()
{
   auto expiry = ngtcp2_conn_get_expiry(conn_);
   auto now = ngtcp2::util::timestamp();

   asio::steady_timer::duration delay =
      expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};

   timer_.expires_after(delay);
   timer_.async_wait([self = weak_from_this()](const boost::system::error_code& ec)
   {
      if (ec)
         return;
      if (auto handler = self.lock())
         handler->handle_expiry();
   });
}

int QuicHandler::handle_expiry()
{
   auto now = ngtcp2::util::timestamp();
   if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
   {
      logw("[{}] ngtcp2_conn_handle_expiry: {}", log_prefix_, ngtcp2_strerror(rv));
      ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }
   if (auto rv = write_streams(); rv != 0)
      return rv;
   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

int QuicHandler::handle_error(int /*rv*/)
{
   // First-slice: flag the connection closed and drop it. No CONNECTION_CLOSE for now --
   // the client will time out. Follow-up commits will add draining/closing periods.
   closed_ = true;
   return -1;
}

// -------------------------------------------------------------------------------------------------
// ngtcp2 callback implementations
// -------------------------------------------------------------------------------------------------

int QuicHandler::cb_handshake_completed(ngtcp2_conn*, void* user)
{
   auto self = static_cast<QuicHandler*>(user);
   logi("[{}] TLS handshake complete", self->log_prefix_);
   if (self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int QuicHandler::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                     uint64_t /*offset*/, const uint8_t* data, size_t datalen,
                                     void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   if (!self->h3_)
      return 0;

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

int QuicHandler::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t /*offset*/,
                                             uint64_t datalen, void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_add_ack_offset(self->h3_, stream_id, datalen); rv != 0)
   {
      loge("[{}] nghttp3_conn_add_ack_offset: {}", self->log_prefix_, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int QuicHandler::cb_stream_open(ngtcp2_conn*, int64_t /*stream_id*/, void* /*user*/) { return 0; }

int QuicHandler::cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                 uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
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

void QuicHandler::cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
   {
      // RAND_bytes failure is very unusual; fall back to something so we don't return
      // uninitialized memory.
      std::memset(dest, 0, destlen);
   }
}

int QuicHandler::cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token,
                                          size_t cidlen, void* user)
{
   auto self = static_cast<QuicHandler*>(user);
   if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   cid->datalen = cidlen;
   if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   self->server_.associate_quic_cid(*cid, self);
   return 0;
}

int QuicHandler::cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user)
{
   auto self = static_cast<QuicHandler*>(user);
   self->server_.dissociate_quic_cid(*cid);
   return 0;
}

int QuicHandler::cb_extend_max_remote_streams_bidi(ngtcp2_conn*, uint64_t /*max_streams*/,
                                                   void* /*user*/)
{
   return 0;
}

int QuicHandler::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                        void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_shutdown_stream_read(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_shutdown_stream_read({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   (void)app_error_code;
   return 0;
}

int QuicHandler::cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t /*final_size*/,
                                 uint64_t /*app_error_code*/, void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
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

int QuicHandler::cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t /*max_data*/,
                                           void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
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

int QuicHandler::cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user)
{
   if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
      return 0;
   auto self = static_cast<QuicHandler*>(user);
   if (!self->h3_ && self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

// -------------------------------------------------------------------------------------------------
// nghttp3 setup + callbacks
// -------------------------------------------------------------------------------------------------

int QuicHandler::setup_http3()
{
   if (h3_)
      return 0;

   nghttp3_callbacks h3cb{};
   h3cb.stream_close = &QuicHandler::h3_cb_stream_close;
   h3cb.recv_data = &QuicHandler::h3_cb_recv_data;
   h3cb.deferred_consume = &QuicHandler::h3_cb_deferred_consume;
   h3cb.begin_headers = &QuicHandler::h3_cb_begin_headers;
   h3cb.recv_header = &QuicHandler::h3_cb_recv_header;
   h3cb.end_headers = &QuicHandler::h3_cb_end_headers;
   h3cb.end_stream = &QuicHandler::h3_cb_end_stream;
   h3cb.stop_sending = &QuicHandler::h3_cb_stop_sending;
   h3cb.reset_stream = &QuicHandler::h3_cb_reset_stream;

   nghttp3_settings settings;
   nghttp3_settings_default(&settings);
   settings.qpack_max_dtable_capacity = 4096;
   settings.qpack_blocked_streams = 100;

   if (auto rv = nghttp3_conn_server_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
   {
      loge("[{}] nghttp3_conn_server_new: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   auto params = ngtcp2_conn_get_local_transport_params(conn_);
   nghttp3_conn_set_max_client_streams_bidi(h3_, params->initial_max_streams_bidi);

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
   return 0;
}

// -------------------------------------------------------------------------------------------------

namespace
{
nghttp3_nv make_nv(std::string_view name, std::string_view value)
{
   nghttp3_nv nv{};
   nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
   nv.namelen = name.size();
   nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
   nv.valuelen = value.size();
   nv.flags = NGHTTP3_NV_FLAG_NONE;
   return nv;
}

nghttp3_ssize response_read_data(nghttp3_conn*, int64_t /*stream_id*/, nghttp3_vec* vec,
                                 size_t veccnt, uint32_t* pflags, void* /*user*/,
                                 void* stream_user)
{
   auto body = static_cast<std::string_view*>(stream_user);
   if (veccnt == 0)
      return 0;
   vec[0].base = reinterpret_cast<uint8_t*>(const_cast<char*>(body->data()));
   vec[0].len = body->size();
   *pflags |= NGHTTP3_DATA_FLAG_EOF;
   return 1;
}
} // namespace

int QuicHandler::submit_response(int64_t stream_id)
{
   static const auto content_length = std::to_string(kResponseBody.size());
   static thread_local std::string_view body_view = kResponseBody;

   std::array<nghttp3_nv, 4> nva{
      make_nv(":status", "200"),
      make_nv("server", "anyhttp-quic/0.1"),
      make_nv("content-type", "text/plain; charset=utf-8"),
      make_nv("content-length", content_length),
   };

   nghttp3_data_reader dr{};
   dr.read_data = response_read_data;

   if (auto rv = nghttp3_conn_set_stream_user_data(h3_, stream_id, &body_view); rv != 0)
   {
      loge("[{}] nghttp3_conn_set_stream_user_data({}): {}", log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return -1;
   }

   if (auto rv = nghttp3_conn_submit_response(h3_, stream_id, nva.data(), nva.size(), &dr);
       rv != 0)
   {
      loge("[{}] nghttp3_conn_submit_response({}): {}", log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return -1;
   }
   logi("[{}.{}] submitted hardcoded 200 OK ({} bytes)", log_prefix_, stream_id,
        kResponseBody.size());
   return 0;
}

// -------------------------------------------------------------------------------------------------
// nghttp3 callback implementations
// -------------------------------------------------------------------------------------------------

int QuicHandler::h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                    void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   logd("[{}.{}] h3 stream closed (app_error=0x{:x})", self->log_prefix_, stream_id,
        app_error_code);
   if (ngtcp2_conn_is_server(self->conn_))
      ngtcp2_conn_extend_max_streams_bidi(self->conn_, 1);
   return 0;
}

int QuicHandler::h3_cb_recv_data(nghttp3_conn*, int64_t /*stream_id*/, const uint8_t* /*data*/,
                                 size_t datalen, void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   // We accept request body bytes but discard them for the first slice.
   ngtcp2_conn_extend_max_offset(self->conn_, datalen);
   return 0;
}

int QuicHandler::h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                        void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, nconsumed);
   ngtcp2_conn_extend_max_offset(self->conn_, nconsumed);
   return 0;
}

int QuicHandler::h3_cb_begin_headers(nghttp3_conn*, int64_t /*stream_id*/, void* /*user*/, void*)
{
   return 0;
}

int QuicHandler::h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t /*token*/,
                                   nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t /*flags*/,
                                   void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   auto n = nghttp3_rcbuf_get_buf(name);
   auto v = nghttp3_rcbuf_get_buf(value);
   logd("[{}.{}]   {}: {}", self->log_prefix_, stream_id,
        std::string_view{reinterpret_cast<const char*>(n.base), n.len},
        std::string_view{reinterpret_cast<const char*>(v.base), v.len});
   return 0;
}

int QuicHandler::h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user,
                                   void*)
{
   auto self = static_cast<QuicHandler*>(user);
   logi("[{}.{}] end of headers", self->log_prefix_, stream_id);
   return self->submit_response(stream_id);
}

int QuicHandler::h3_cb_end_stream(nghttp3_conn*, int64_t /*stream_id*/, void* /*user*/, void*)
{
   return 0;
}

int QuicHandler::h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                    void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   ngtcp2_conn_shutdown_stream_read(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

int QuicHandler::h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                    void* user, void*)
{
   auto self = static_cast<QuicHandler*>(user);
   ngtcp2_conn_shutdown_stream_write(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

// =================================================================================================
// Packet dispatch: this is what turns raw UDP datagrams from the shared socket into
// per-connection QuicHandler instances, creating new ones on first Initial packet.
// =================================================================================================

namespace
{

std::optional<ngtcp2::Address> to_ngtcp2_address(const sockaddr_storage& src, socklen_t len)
{
   ngtcp2::Address addr{};
   if (len > sizeof(addr.su))
      return std::nullopt;
   std::memcpy(&addr.su, &src, len);
   addr.len = len;
   return addr;
}

} // namespace

// -------------------------------------------------------------------------------------------------
// Server::Impl QUIC glue.
// The Impl gets a couple of new members (see server_impl.hpp) to hold the CID->handler map.
// -------------------------------------------------------------------------------------------------

void Server::Impl::associate_quic_cid(const ngtcp2_cid& cid, QuicHandler* h)
{
   m_quic_handlers.emplace(cid_key(cid), h->shared_from_this());
}

void Server::Impl::dissociate_quic_cid(const ngtcp2_cid& cid)
{
   m_quic_handlers.erase(cid_key(cid));
}

// -------------------------------------------------------------------------------------------------

int Server::Impl::udp_on_read(Endpoint& ep)
{
   ngtcp2::sockaddr_union su;
   std::array<uint8_t, 64_k> buf;
   ngtcp2_pkt_info pi{};

   iovec msg_iov{buf.data(), buf.size()};
   msghdr msg{};
   msg.msg_name = &su;
   msg.msg_iov = &msg_iov;
   msg.msg_iovlen = 1;

   uint8_t
      msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(int))];
   msg.msg_control = msg_ctrl;

   for (size_t pktcnt = 0; pktcnt < 32; ++pktcnt)
   {
      msg.msg_namelen = sizeof(su);
      msg.msg_controllen = sizeof(msg_ctrl);

      auto nread = recvmsg(ep.fd, &msg, 0);
      if (nread == -1)
      {
         if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOTCONN)
            loge("recvmsg: {}", strerror(errno));
         return 0;
      }

      if (nread < 22) // shortest possible QUIC packet
         continue;

      auto local_addr = ngtcp2::msghdr_get_local_addr(&msg, su.storage.ss_family);
      if (!local_addr)
      {
         logw("could not obtain local address from cmsg");
         continue;
      }
      ngtcp2::set_port(*local_addr, ep.addr);
      ep.addr = *local_addr;

      auto data = std::span<const uint8_t>{buf.data(), static_cast<size_t>(nread)};

      ngtcp2_version_cid vc;
      auto rv = ngtcp2_pkt_decode_version_cid(&vc, data.data(), data.size(), QUIC_SCIDLEN);
      if (rv != 0)
      {
         if (rv != NGTCP2_ERR_VERSION_NEGOTIATION)
            logw("could not decode version/cid: {}", ngtcp2_strerror(rv));
         // First slice: no version negotiation response yet.
         continue;
      }

      auto key = cid_key(vc.dcid, vc.dcidlen);
      auto it = m_quic_handlers.find(key);

      if (it == m_quic_handlers.end())
      {
         ngtcp2_pkt_hd hd;
         if (auto arv = ngtcp2_accept(&hd, data.data(), data.size()); arv != 0)
         {
            // Unexpected packet for an unknown CID -- silently drop.
            continue;
         }

         auto remote = to_ngtcp2_address(su.storage, msg.msg_namelen);
         if (!remote)
         {
            logw("unsupported remote address family");
            continue;
         }

         auto handler =
            std::make_shared<QuicHandler>(*this, ep, *remote);
         if (handler->init(hd.dcid, hd.scid, hd.version, pi, data) != 0)
         {
            continue;
         }

         //
         // Register the handler under both the client's original DCID and every
         // server-side SCID ngtcp2 knows about. Without the former, a client
         // retransmitting its Initial before receiving our reply would spawn a
         // second handler; without the latter, connection-ID rotation would strand
         // packets on unregistered CIDs.
         //
         m_quic_handlers.emplace(std::move(key), handler);
         std::array<ngtcp2_cid, 8> scids;
         auto num_scid = ngtcp2_conn_get_scid(handler->conn(), nullptr);
         if (num_scid <= scids.size())
         {
            ngtcp2_conn_get_scid(handler->conn(), scids.data());
            for (size_t i = 0; i < num_scid; ++i)
               m_quic_handlers.emplace(cid_key(scids[i]), handler);
         }
      }
      else
      {
         auto handler = it->second;
         auto remote = to_ngtcp2_address(su.storage, msg.msg_namelen);
         if (!remote)
            continue;
         if (handler->on_read(pi, data, *remote) != 0 && handler->closed())
         {
            m_quic_handlers.erase(it);
         }
      }
   }
   return 0;
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Server::Impl::udp_receive_loop()
{
   for (;;)
   {
      boost::system::error_code ec;
      co_await m_udp_socket->async_wait(boost::asio::socket_base::wait_read,
                                        redirect_error(use_awaitable, ec));
      if (ec)
      {
         if (ec == boost::asio::error::operation_aborted)
            logi("UDP receive: {}", ec.message());
         else
            logw("UDP receive: {}", ec.message());
         co_return;
      }

      Endpoint ep{};
      ep.fd = m_udp_socket->native_handle();
      auto local = m_udp_socket->local_endpoint();
      auto data = local.data();
      std::memcpy(&ep.addr.su, data, local.size());
      ep.addr.len = local.size();

      udp_on_read(ep);
   }
}

// =================================================================================================

} // namespace anyhttp::server
