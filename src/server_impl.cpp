#include "anyhttp/server_impl.hpp"
#include "anyhttp/beast_session.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/detect_http2.hpp"

#include "anyhttp/beast_session.hpp"
#include "anyhttp/detail/nghttp2_session_details.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::server
{

// =================================================================================================

#if 0
Request::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Request: ctor\x1b[0m"); }
Request::Impl::~Impl() { logd("\x1b[35mServer::Request: dtor\x1b[0m"); }

Response::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Response: ctor\x1b[0m"); }
Response::Impl::~Impl() { logd("\x1b[35mServer::Response: dtor\x1b[0m"); }
#else
Request::Impl::Impl() noexcept = default;
Request::Impl::~Impl() = default;

Response::Impl::Impl() noexcept = default;
Response::Impl::~Impl() = default;
#endif

// =================================================================================================

Server::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
#if !defined(NDEBUG)
   spdlog::set_level(spdlog::level::debug);
#else
   spdlog::set_level(spdlog::level::info);
#endif
   logi("Server: ctor");
   listen();
}

Server::Impl::~Impl()
{
   logi("Server: dtor");
   assert(m_stopped);
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::run()
{
   // co_spawn(m_executor, listen_loop(), asio::consign(detached, shared_from_this()));
   co_spawn(m_executor, listen_loop(),
            [self = shared_from_this()](const std::exception_ptr& ex)
            {
               if (ex)
                  logw("server run: {}", what(ex));
               else
                  logi("server run: done");
            });
}

void Server::Impl::destroy()
{
   logi("Server: destroy");
   m_stopped = true;
   m_acceptor.close(); // breaks listen_loop()
}

// =================================================================================================

//
// https://nghttp2.org/documentation/tutorial-server.html
//
static unsigned char next_proto_list[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

unsigned int next_proto_list_len = sizeof(next_proto_list);
static int next_proto_cb(SSL* s, const unsigned char** data, unsigned int* len, void* arg)
{
   *data = next_proto_list;
   *len = (unsigned int)next_proto_list_len;
   return SSL_TLSEXT_ERR_OK;
}

static int alpn_select_proto_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                const unsigned char* in, unsigned int inlen, void* arg)
{
   int rv = nghttp2_select_next_protocol((unsigned char**)out, outlen, in, inlen);
   switch (rv)
   {
   case 0:
      return SSL_TLSEXT_ERR_OK; // http/1.1
   case 1:
      return SSL_TLSEXT_ERR_OK; // h2
   case -1:
   default:
      return SSL_TLSEXT_ERR_NOACK;
   }
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Server::Impl::handleConnection(ip::tcp::socket socket)
{
   logi("[{}] new connection", normalize(socket.remote_endpoint()));
   auto executor = co_await boost::asio::this_coro::executor;

   socket.set_option(ip::tcp::no_delay(true));

   //
   // Playing with socket buffer sizes... Doesn't seem to do any good.
   //
   using sb = boost::asio::socket_base;
   sb::send_buffer_size send_buffer_size;
   sb::receive_buffer_size receive_buffer_size;
   socket.get_option(send_buffer_size);
   socket.get_option(receive_buffer_size);
   logd("[{}] socket buffer sizes: send={} receive={}", normalize(socket.remote_endpoint()),
        send_buffer_size.value(), receive_buffer_size.value());
#if 0
   socket.set_option(sb::send_buffer_size(8192));
   socket.set_option(sb::receive_buffer_size(8192)); // makes 'PostRange' testcases very slow
#endif

   const auto prefix = normalize(socket.remote_endpoint());

   auto buffer = boost::beast::flat_buffer();

   //
   // try to detect TLS
   //
#if 1
   std::shared_ptr<Session::Impl> session;
   std::optional<asio::ssl::stream<asio::ip::tcp::socket>> sslStream;
   if (co_await async_detect_ssl_awaitable(socket, buffer, deferred))
   {
      logi("[{}] detected TLS client hello, {} bytes in buffer", prefix, buffer.size());

      asio::ssl::context ctx{asio::ssl::context::tlsv13};
      SSL_CTX_set_next_protos_advertised_cb(ctx.native_handle(), next_proto_cb, NULL);
      SSL_CTX_set_alpn_select_cb(ctx.native_handle(), alpn_select_proto_cb, NULL);
      ctx.use_certificate_chain_file("etc/darkbase-chain.pem");
      ctx.use_private_key_file("etc/darkbase-key.pem", asio::ssl::context::pem);

      sslStream.emplace(std::move(socket), ctx);
      auto n = co_await sslStream->async_handshake(asio::ssl::stream_base::server, buffer.data(),
                                                   asio::deferred);
      buffer.consume(n);

      //
      // ALPN
      //
      std::string_view alpn;
      {
         const unsigned char* data;
         unsigned int len;
         SSL_get0_alpn_selected(sslStream->native_handle(), &data, &len);
         if (data)
            alpn = std::string_view(reinterpret_cast<const char*>(data), len);
      }

      if (alpn == "h2")
         session =
            std::make_shared<nghttp2::ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>> //
            (*this, executor, std::move(*sslStream));
      else if (alpn == "http/1.1")
         session =
            std::make_shared<beast_impl::ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>> //
            (*this, executor, std::move(*sslStream));
   }
#endif

   //
   // detect HTTP2 client preface, fallback to HTTP/1.1 if not found
   //
   if (session)
      ; // SSL session, see above
   else if (co_await async_detect_http2_client_preface(socket, buffer, deferred))
   {
      logi("[{}] detected HTTP2 client preface, {} bytes in buffer", prefix, buffer.size());
#if 0
      session = std::make_shared<nghttp2::ServerSession<boost::beast::tcp_stream>> //
         (*this, executor, boost::beast::tcp_stream(std::move(socket)));
#else
      session = std::make_shared<nghttp2::ServerSession<asio::ip::tcp::socket>> //
         (*this, executor, std::move(socket));
#endif
   }
   else
   {
      logi("[{}] no HTTP2 client preface, assuming HTTP/1.x", prefix);
      session = std::make_shared<beast_impl::ServerSession<boost::beast::tcp_stream>> //
         (*this, executor, boost::beast::tcp_stream(std::move(socket)));
   }

   m_sessions.emplace(session);

   co_await session->do_session(std::move(buffer));

   logi("[{}] session finished", prefix);
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::listen()
{
   auto& acceptor = m_acceptor;

   boost::system::error_code ec;
   ip::address address = ip::address::from_string(config().listen_address, ec);
   if (ec)
      logw("Server: error resolving '{}': {}", config().listen_address, ec.what());

   ip::tcp::endpoint endpoint(address, config().port);
   if (endpoint.protocol() == ip::tcp::v6())
      std::ignore = acceptor.set_option(ip::v6_only(false), ec);

   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(endpoint);
   acceptor.listen();

   endpoint = acceptor.local_endpoint();
   logi("Server: listening on {}", endpoint);
}

// -------------------------------------------------------------------------------------------------

/**
 * Typically, a listen loop "spawns" a new thread of execution for each connection it accepts.
 * Doing that in a "detached" fassion violates the principles of structured concurrency, as we
 * don't have a clear way of cancelling those threads.
 *
 * To solve this, we always use spawn with a callback and use that to wait for pending tasks.
 *
 * https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3149r5.html#listener-loop-in-an-http-server
 *
 */
awaitable<void> Server::Impl::listen_loop()
{
   auto executor = co_await boost::asio::this_coro::executor;

   size_t sessionCounter = 0;
   for (;;)
   {
      auto [ec, socket] = co_await m_acceptor.async_accept(as_tuple(deferred));
      if (ec == boost::system::errc::operation_canceled)
      {
         logi("accept: {}", ec.message());
         break;
      }
      else if (ec)
      {
         logw("accept: {}", ec.message());
         break;
      }

      auto ep = normalize(socket.remote_endpoint());

      //
      // Without something like a "nursery" or "async_scope", spwaning a tasks detaches it from
      // the owning class without any means to join it.
      //
      ++sessionCounter;
      co_spawn(
         boost::asio::make_strand(executor),  // put each connection on a strand
         // executor,
         [&]() mutable { //
            return handleConnection(std::move(socket));
         },
         [&](const std::exception_ptr& ex) mutable
         {
            --sessionCounter;
            if (ex)            
               logw("{} {}", ep, what(ex));
            else
               logi("{} session finished, {} sessions left", ep, sessionCounter);
         });
   }

   const auto waitingFor = sessionCounter;
   logi("listen loop terminated, waiting for {} sessions...", waitingFor);

   while (sessionCounter)
   {
      for (auto& session : m_sessions)
         session->destroy();

      m_sessions.clear();
      co_await post(executor, asio::deferred);
   }

   logi("listen loop terminated, waiting for {} sessions... done", waitingFor);
}

// =================================================================================================

} // namespace anyhttp::server
