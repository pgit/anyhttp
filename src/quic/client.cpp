//
// anyhttp QUIC client.
//
// Almost all of it is shared with the server: `QuicClientSession` is a `quic::QuicSession` (see
// anyhttp/quic/session.hpp) that knows how packets reach it and how it is torn down. What runs on
// top is the QuicApplication the caller of async_connect() makes, HTTP/3 for one
// (src/h3/client.cpp).
//
// What is genuinely client-side here: the TLS client context, one `connect()`ed UDP socket per
// session -- there is exactly one peer, so no connection-ID demux table is needed, unlike the
// server's shared socket -- and the receive loop feeding it.
//
// Not yet implemented: 0-RTT, connection migration, GSO/ECN, retry tokens, graceful (multi-PTO)
// close.
//

#include "anyhttp/quic/client.hpp"
#include "anyhttp/client_impl.hpp" // IWYU pragma: keep
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"
#include "anyhttp/quic/common.hpp"
#include "anyhttp/quic/session.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using namespace boost::asio;

using anyhttp::quic::format_hex;

namespace anyhttp::quic
{

// =================================================================================================
// Free-standing helpers
// =================================================================================================

namespace
{

//
// The client-role SSL_CTX for one outgoing QUIC connection. It is built per connection
// because the trust store and the ALPN come from the ClientConfig; SSL_new() takes a reference,
// so the context can go once the session's SSL exists.
//
struct TlsClientContext
{
   explicit TlsClientContext(const ClientConfig& config)
   {
      ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new");

      QuicSession::configure_tls_context(ctx, false);

      if (config.alpn.empty() || config.alpn.size() > 255)
         throw std::invalid_argument(std::format("invalid ALPN '{}'", config.alpn));
      std::string alpn;
      alpn += static_cast<char>(config.alpn.size());
      alpn += config.alpn;
      SSL_CTX_set_alpn_protos(ctx, reinterpret_cast<const unsigned char*>(alpn.data()),
                              static_cast<unsigned int>(alpn.size()));

      if (config.tls_ca_file.empty())
      {
         if (SSL_CTX_set_default_verify_paths(ctx) != 1)
            throw std::runtime_error(std::format("SSL_CTX_set_default_verify_paths: {}",
                                                 ERR_error_string(ERR_get_error(), nullptr)));
      }
      else if (SSL_CTX_load_verify_locations(ctx, config.tls_ca_file.c_str(), nullptr) != 1)
         throw std::runtime_error(std::format("SSL_CTX_load_verify_locations: {}: {}",
                                              config.tls_ca_file,
                                              ERR_error_string(ERR_get_error(), nullptr)));

      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
   }

   ~TlsClientContext()
   {
      if (ctx)
         SSL_CTX_free(ctx);
   }

   TlsClientContext(const TlsClientContext&) = delete;
   TlsClientContext& operator=(const TlsClientContext&) = delete;

   SSL_CTX* ctx = nullptr;
};

} // namespace

// =================================================================================================
// QuicClientSession: the client's end of the shared QUIC implementation.
// =================================================================================================

class QuicClientSession : public QuicSession
{
public:
   explicit QuicClientSession(const Executor& executor);
   ~QuicClientSession() override;

   //
   // Session::Impl
   //
   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;
   Task<void> do_session(Buffer&& data) override;
   void destroy() noexcept override;

   //
   // Connect-time setup, with the application `factory` makes. Returns 0 on success.
   //
   int init(UdpEndpoint remote, const ClientConfig& config, const ApplicationFactory& factory);

   //
   // Awaited by async_connect() before handing the session back to the caller. Fires once the
   // QUIC handshake has progressed far enough to bring up the application (see
   // start_application()), or once the connection has failed/closed before getting that far --
   // in which case `ready()` is still false and the caller should synthesize an error.
   //
   auto wait_ready() { return ready_.wait(); }
   bool ready() const noexcept { return application_ready(); }

protected:
   int handle_error(int rv) override;
   int send_datagrams(const ngtcp2_path& path, std::span<const uint8_t> data,
                      size_t gso_size) override;
   void on_application_ready() override { signal_ready(); }

private:
   int on_read(std::span<const uint8_t> data);
   void close();
   void signal_ready();

private:
   UdpSocket socket_;
   Event ready_; // see wait_ready()
};

// =================================================================================================
// QuicClientSession implementation
// =================================================================================================

QuicClientSession::QuicClientSession(const Executor& executor)
   : QuicSession(executor), socket_(io::make_udp_socket(get_executor()))
{
}

QuicClientSession::~QuicClientSession()
{
   //
   // Tear the streams down while this object is still whole: destroying a stream fires pending
   // handlers, which reach back into the session.
   //
   clear_streams();
   mlogd("session deleted");
}

// -------------------------------------------------------------------------------------------------

int QuicClientSession::init(UdpEndpoint remote, const ClientConfig& config,
                            const ApplicationFactory& factory)
{
   TlsClientContext tls{config}; // may throw

   if (auto ec = io::open(socket_, remote))
   {
      loge("QuicClientSession::init: open: {}", ec.message());
      return -1;
   }
   if (auto ec = io::connect(socket_, remote))
   {
      loge("QuicClientSession::init: connect: {}", ec.message());
      return -1;
   }
   auto local = io::to_sockaddr(io::local_endpoint(socket_)); // may throw
   auto peer = io::to_sockaddr(remote);

   log_prefix_ = quic::log_prefix(Role::client, config.alpn, peer.data(), peer.size);
   mlogd("session created");

   scid_.datalen = 17;
   if (RAND_bytes(scid_.data, static_cast<int>(scid_.datalen)) != 1)
   {
      mloge("init: RAND_bytes for SCID failed");
      return -1;
   }
   ngtcp2_cid dcid{};
   dcid.datalen = QUIC_SCIDLEN;
   if (RAND_bytes(dcid.data, static_cast<int>(dcid.datalen)) != 1)
   {
      mloge("init: RAND_bytes for DCID failed");
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   fill_callbacks(callbacks);
   callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
   callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;

   ngtcp2_settings settings;
   ngtcp2_transport_params params;
   fill_settings(settings, params, config.idle_timeout);
   set_application(factory(*this));
   application_->fill_transport_params(params);

   ngtcp2_path path{
      {local.data(), local.size},
      {peer.data(), peer.size},
      nullptr,
   };

   if (auto rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid_, &path, NGTCP2_PROTO_VER_V1,
                                        &callbacks, &settings, &params, nullptr, this);
       rv != 0)
   {
      mloge("ngtcp2_conn_client_new: {}", ngtcp2_strerror(rv));
      return -1;
   }

   if (setup_tls(tls.ctx, false /* client */) != 0)
      return -1;

   //
   // The certificate has to be valid for the origin, the host of the URL -- also when `remote` is
   // an alternative service it advertised (RFC 7838, section 2.1). An IP address is matched
   // against the certificate's IP SANs and not sent as SNI, which is for host names only.
   //
   const std::string& host = config.host;
   auto* param = SSL_get0_param(ssl_);
   error_code not_an_ip;
   io::make_address(host, not_an_ip);
   if (!not_an_ip ? X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1
                  : X509_VERIFY_PARAM_set1_host(param, host.data(), host.size()) != 1 ||
                       SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1)
   {
      mloge("init: can't verify certificates for '{}'", host);
      return -1;
   }

   mlogd("connecting, scid={}", format_hex(scid_.data, scid_.datalen));
   return 0;
}

// -------------------------------------------------------------------------------------------------

Task<void> QuicClientSession::do_session(Buffer&&)
{
   if (flush_write() != 0)
   {
      signal_ready();
      co_return;
   }

   std::array<uint8_t, 64_k> buf;
   for (;;)
   {
      auto [ec, n] = co_await io::receive(socket_, asio::buffer(buf));
      if (ec)
      {
         if (ec != errc::operation_canceled)
            mlogw("receive: {}", ec.message());
         break;
      }

      if (on_read({buf.data(), n}) != 0)
         break; // handle_error() already tore things down.

      //
      // close() may have run from inside on_read(): handing a response chunk or EOF to the
      // application resumes its coroutine, which may drop the last reference to the Session
      // right there. Its socket_.cancel() then found no receive pending -- we are between two
      // of them -- so nothing would stop us from arming a fresh one that no peer will ever
      // complete. The server, already draining because it got our CONNECTION_CLOSE, does not
      // even answer it.
      //
      if (closed())
         break;
   }

   //
   // The receive loop only ever ends because this connection is over: the socket errored out (ICMP
   // reporting the peer's port unreachable, say), close() cancelled it, or on_read() hit a protocol
   // error. Tear the session down in every case -- nothing else is running that could ever complete
   // the requests still waiting on it, so leaving them pending hangs them forever. close() is
   // idempotent, so the paths that already tore things down are unaffected, and it signals ready to
   // unblock a waiter whose handshake never finished.
   //
   close();
   co_return;
}

void QuicClientSession::destroy() noexcept { close(); }

void QuicClientSession::close()
{
   if (std::exchange(closed_, true))
      return;

   //
   // The connection is going away (user-initiated destroy(), or a protocol/transport error via
   // handle_error()) -- fail every request that hasn't completed yet instead of leaving its
   // async_get_response()/async_read_some() hanging forever.
   //
   fail_streams(make_error_code(errc::connection_reset));

   //
   // An idle-timed-out (or dropped) connection is discarded silently: RFC 9000 has no
   // CONNECTION_CLOSE for it, and there is nobody left listening anyway -- writing one would
   // just put a packet on a path whose peer has been gone for a full idle period.
   //
   const bool silent = last_error_.type == NGTCP2_CCERR_TYPE_IDLE_CLOSE ||
                       last_error_.type == NGTCP2_CCERR_TYPE_DROP_CONN;

   if (conn_ && !silent && !ngtcp2_conn_in_closing_period(conn_) &&
       !ngtcp2_conn_in_draining_period(conn_))
   {
      std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> closebuf;
      ngtcp2_path_storage ps;
      if (auto packet = write_connection_close(closebuf, ps); !packet.empty())
         send_datagrams(ps.path, packet, packet.size());
   }

   io::cancel(socket_);
   timer_.cancel();
   signal_ready();
}

void QuicClientSession::signal_ready() { ready_.set(); }

int QuicClientSession::handle_error(int /*rv*/)
{
   // X509_V_ERR_INVALID_CALL: the handshake never got as far as verifying anything
   if (ssl_)
      if (auto result = SSL_get_verify_result(ssl_);
          result != X509_V_OK && result != X509_V_ERR_INVALID_CALL)
         mlogw("server certificate: {}", X509_verify_cert_error_string(result));

   close();
   return -1;
}

// -------------------------------------------------------------------------------------------------

//
// The connected socket has exactly one peer, so the path is of no interest here. What
// ngtcp2_conn_write_aggregate_pkt2() produced may be several QUIC packets, all but the last
// exactly `gso_size` bytes long -- without UDP_SEGMENT (which the server uses on its shared,
// unconnected socket) they go out one send() at a time.
//
int QuicClientSession::send_datagrams(const ngtcp2_path& /*path*/, std::span<const uint8_t> data,
                                      size_t gso_size)
{
   while (!data.empty())
   {
      auto len = std::min(gso_size, data.size());
      auto ec = io::send(socket_, asio::buffer(data.data(), len));
      if (ec && ec != errc::operation_would_block && ec != errc::resource_unavailable_try_again)
      {
         mlogw("send: {}", ec.message());
         return 0; // best-effort; ngtcp2 will retransmit
      }
      data = data.subspan(len);
   }
   return 0;
}

int QuicClientSession::on_read(std::span<const uint8_t> data)
{
   ngtcp2_pkt_info pi{};
   auto* path = ngtcp2_conn_get_path(conn_);
   if (QuicSession::on_read(*path, pi, data) != 0)
      return -1;

   //
   // Unlike the server, which reads a whole batch of datagrams before answering it in one pass,
   // there is only ever one packet in flight here -- flush right away.
   //
   return flush_write();
}

// -------------------------------------------------------------------------------------------------

void QuicClientSession::async_submit(SubmitHandler&& handler, std::string_view method,
                                     boost::urls::url url, const Fields& headers)
{
   if (closed() || !ready())
   {
      mloge("async_submit: session not ready");
      std::move(handler)(errors::canceled, client::Request{nullptr});
      return;
   }
   application_->async_submit(std::move(handler), method, std::move(url), headers);
}

// =================================================================================================

Task<std::shared_ptr<QuicSession>> async_connect(Executor executor, std::string host,
                                                 std::string port, ClientConfig config,
                                                 ApplicationFactory factory)
{
   auto [resolved, endpoints] = co_await io::resolve(executor, host, port);
   if (resolved)
      throw_error(resolved);
   if (endpoints.empty())
      throw_error(make_error_code(errc::host_unreachable));
   const UdpEndpoint remote{endpoints.front().address(), endpoints.front().port()};

   auto session = std::make_shared<QuicClientSession>(executor);
   if (session->init(remote, config, factory) != 0)
      throw_error(make_error_code(errc::connection_refused));

   std::shared_ptr<Session::Impl> impl = session;

   launch(executor, impl->do_session(Buffer{}),
          [impl, prefix = session->log_prefix()](const std::exception_ptr& ex) mutable {
             if (ex)
                logw("[{}] client run: {}", prefix, what(ex));
             else
                logi("[{}] client run: done", prefix);
             impl.reset();
          });

   //
   // The wait ends when the handshake has gotten far enough, when the connection has failed or
   // closed before that, or when the caller cancels. Only ready() tells these apart.
   //
   auto [ec] = co_await session->wait_ready();
   if (!session->ready())
      throw_error(make_error_code(errc::connection_refused));

   co_return session;
}

// =================================================================================================

} // namespace anyhttp::quic
