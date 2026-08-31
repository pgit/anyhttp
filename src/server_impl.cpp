#include "anyhttp/server_impl.hpp"

#include "anyhttp/any_async_stream.hpp"
#include "anyhttp/detect_ssl.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h1_backend.hpp"
#include "anyhttp/h2_backend.hpp"
#include "anyhttp/h2_detect.hpp"
#include "anyhttp/h3_backend.hpp"
#include "anyhttp/tls.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/as_single.hpp>
#include <boost/asio/immediate.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <span>
#include <string_view>

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
   logi("Server: ctor");
   listen_tcp();

   //
   // HTTP/3 shares the endpoint the TCP acceptor is listening on, so it has to be set up after
   // listen_tcp(): with port=0 the actual port is only known once the acceptor is bound.
   //
   auto tcp_ep = m_acceptor->local_endpoint();
   m_http3 = make_http3_server(*this, ip::udp::endpoint{tcp_ep.address(), tcp_ep.port()});
}

// -------------------------------------------------------------------------------------------------

/**
 * A shared pointer is captured in the completion handler of the spawned tasks. This way, we
 * make sure it stays around long enough, even if the user has already deleted it.
 *
 * Most of the cleanup is done at the end of listen_loop(), which collects all the shared pointers.
 */
void Server::Impl::start()
{
   co_spawn(m_executor, tcp_accept_loop(), [self = shared_from_this()](const std::exception_ptr& ex)
   {
      if (ex)
         logw("TCP accept loop: {}", what(ex));
      else
         logi("TCP accept loop: done");
   });

   if (m_http3)
      m_http3->start();
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::destroy()
{
   logi("Server: destroy");

   if (m_acceptor)
      m_acceptor->close(); // breaks listen_loop()

   //
   // Destroy all active sessions (TCP and QUIC) so their timers and async operations are
   // cancelled, allowing the io_context to drain. QUIC sessions send a final CONNECTION_CLOSE
   // as part of destroy() -- through their own dup()ed fd, so closing the shared UDP socket
   // below doesn't race with it. Setting m_destroyed under the same lock is what keeps
   // process_quic_batch(), running on some session strand, from registering a new session
   // after this loop has run: it re-checks the flag under the lock before inserting.
   //
   {
      auto lock = std::lock_guard(m_sessionMutex);
      m_destroyed = true;
      for (auto& session : m_sessions)
         session->destroy();
   }

   if (m_http3)
      m_http3->destroy();
}

// -------------------------------------------------------------------------------------------------

Server::Impl::~Impl()
{
   logi("Server: dtor");
   assert(m_destroyed);
}

// -------------------------------------------------------------------------------------------------

bool Server::Impl::add_session(std::shared_ptr<Session::Impl> session)
{
   auto lock = std::lock_guard(m_sessionMutex);
   if (m_destroyed)
      return false;
   m_sessions.emplace(std::move(session));
   return true;
}

void Server::Impl::remove_session(const std::shared_ptr<Session::Impl>& session)
{
   auto lock = std::lock_guard(m_sessionMutex);
   m_sessions.erase(session);
}

// =================================================================================================

void Server::Impl::listen_tcp()
{
   assert(m_acceptor);
   auto& acceptor = *m_acceptor;

   boost::system::error_code ec;
   auto address = ip::make_address(config().listen_address, ec);
   if (ec)
      logw("Server: error resolving '{}': {}", config().listen_address, ec.what());

   ip::tcp::endpoint ep(address, config().port);
   if (ep.protocol() == ip::tcp::v6())
      std::ignore = acceptor.set_option(ip::v6_only(false), ec);

   acceptor.open(ep.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(ep);
   acceptor.listen();

   ep = acceptor.local_endpoint();
   logi("Server: TCP listening on {}", ep);
}

// =================================================================================================

//
// The protocols we speak over TLS on TCP, in descending order of preference. HTTP/3 is not in
// here: it is offered on the UDP endpoint instead, see anyhttp/h3_backend.hpp.
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

//
// ALPN, picking the first protocol of ours the client offers -- our preference wins, not the
// client's order.
//
static int alpn_select_proto_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                const unsigned char* in, unsigned int inlen, void* arg)
{
   for (std::string_view wanted : {"h2", "http/1.1"})
   {
      // The wire format is a sequence of length-prefixed, non-empty protocol names.
      for (auto list = std::span{in, inlen}; !list.empty() && list.size() > list[0];
           list = list.subspan(1 + list[0]))
      {
         if (std::string_view{reinterpret_cast<const char*>(&list[1]), list[0]} != wanted)
            continue;

         *out = &list[1];
         *outlen = list[0];
         return SSL_TLSEXT_ERR_OK;
      }
   }

   return SSL_TLSEXT_ERR_NOACK;
}

// -------------------------------------------------------------------------------------------------

class TestStream : public AnyAsyncStream::Impl
{
public:
   TestStream(ip::tcp::socket socket) : socket_(std::move(socket)) {}
   executor_type get_executor() noexcept override { return socket_.get_executor(); }

   ip::tcp::socket& get_socket() final { return socket_; }
   void async_write_impl(ReadWriteHandler handler, ConstBuffers buffers) final
   {
      socket_.async_write_some(buffers, std::move(handler));
   }

   void async_read_impl(ReadWriteHandler handler, MutableBuffers buffers) final
   {
      socket_.async_read_some(buffers, std::move(handler));
   }

private:
   ip::tcp::socket socket_; // the underlying socket, for cancellation
};

// -------------------------------------------------------------------------------------------------

awaitable<void> Server::Impl::handle_connection(ip::tcp::socket socket)
{
   const auto prefix = normalize(socket.remote_endpoint());
   logi("[{}] new connection", prefix);

   // HTTP/2 is very slow without this, and TLS handshake is faster as well.
   socket.set_option(ip::tcp::no_delay(true));

   //
   // Playing with socket buffer sizes... Doesn't seem to do any good.
   //
   using sb = boost::asio::socket_base;
   sb::send_buffer_size send_buffer_size;
   sb::receive_buffer_size receive_buffer_size;
   socket.get_option(send_buffer_size);
   socket.get_option(receive_buffer_size);
   logd("[{}] socket buffer sizes: send={} receive={}", prefix, send_buffer_size.value(),
        receive_buffer_size.value());

   // socket.set_option(sb::send_buffer_size(8192));
   // socket.set_option(sb::receive_buffer_size(8192)); // makes 'PostRange' testcases very slow

   auto executor = co_await boost::asio::this_coro::executor;
   auto buffer = boost::beast::flat_buffer();

   //
   // detect TLS
   //
   std::shared_ptr<Session::Impl> session;
   std::optional<asio::ssl::stream<asio::ip::tcp::socket>> ssl_stream;
   if (co_await async_detect_ssl_awaitable(socket, buffer, deferred))
   {
      logi("[{}] detected TLS client hello, {} bytes in buffer", prefix, buffer.size());

      asio::ssl::context ctx{asio::ssl::context::tlsv13};
      SSL_CTX_set_next_protos_advertised_cb(ctx.native_handle(), next_proto_cb, NULL);
      SSL_CTX_set_alpn_select_cb(ctx.native_handle(), alpn_select_proto_cb, NULL);

      //
      // This is a testing key only. It is not in the repository, but generated at build time
      // by the 'pki' target (see cmake/pki.cmake).
      //
      ctx.use_certificate_chain_file("pki/out/server-chain.pem");
      ctx.use_private_key_file("pki/out/server-key.pem", asio::ssl::context::pem);

      ssl_stream.emplace(std::move(socket), ctx);
      auto n = co_await ssl_stream->async_handshake(asio::ssl::stream_base::server, buffer.data());
      buffer.consume(n);

      //
      // perform ALPN
      //
      std::string_view alpn;
      {
         const unsigned char* data;
         unsigned int len;
         SSL_get0_alpn_selected(ssl_stream->native_handle(), &data, &len);
         if (data)
            alpn = std::string_view(reinterpret_cast<const char*>(data), len);
      }

      logi("[{}] TLS handshake completed: {}", prefix,
           tls_handshake_info(ssl_stream->native_handle()));

      if (alpn == "h2")
         session = nghttp2::make_server_session(*this, executor, std::move(*ssl_stream));
      else if (alpn == "http/1.1")
         session = beast_impl::make_server_session(*this, executor, std::move(*ssl_stream));
   }

   //
   // detect HTTP2 client preface
   //
   else if (co_await async_detect_http2_client_preface(socket, buffer))
   {
      logi("[{}] detected HTTP2 client preface, {} bytes in buffer", prefix, buffer.size());
#if 1
      AnyAsyncStream stream(std::make_unique<TestStream>(std::move(socket)));
      session = nghttp2::make_server_session(*this, executor, std::move(stream));
#else
      session = nghttp2::make_server_session(*this, executor, std::move(socket));
#endif
   }

   //
   // fallback to HTTP/1.1
   //
   else
   {
      logi("[{}] no HTTP2 client preface, assuming HTTP/1.x", prefix);
#if 1
      AnyAsyncStream stream(std::make_unique<TestStream>(std::move(socket)));
      session = beast_impl::make_server_session(*this, executor, std::move(stream));
#else
      session = beast_impl::make_server_session(*this, executor, std::move(socket));
#endif
   }

   //
   // Registration fails only if the server is already being destroyed, in which case this
   // session has to go away right here: nothing else knows about it any more.
   //
   if (!add_session(session))
   {
      logi("[{}] server is shutting down, dropping connection", prefix);
      session->destroy();
      co_return;
   }

   co_await session->do_session(std::move(buffer));
   remove_session(session);

   logi("[{}] session finished", prefix);
}

// -------------------------------------------------------------------------------------------------

/**
 * Typically, an accept loop "spawns" a new thread of execution for each connection it accepts.
 * Doing that in a "detached" fashion violates the principles of structured concurrency, as we
 * don't have a clear way of cancelling those threads.
 *
 * To solve this, we always use spawn with a callback and use that to wait for pending tasks.
 *
 * https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3149r5.html#listener-loop-in-an-http-server
 *
 */
awaitable<void> Server::Impl::tcp_accept_loop()
{
   assert(m_acceptor);
   auto& acceptor = *m_acceptor;
   const auto executor = co_await boost::asio::this_coro::executor;

   //
   // FIXME: sessionCounter and m_sessions are not thread safe, yet
   //
   // The main problem with m_sessions is that the new session is emplaced within
   // handleConnection(), which is already outside this coroutines strand.
   //
   // Maybe the simplest solution is to put a mutex around it...
   //
   size_t sessionCounter = 0;
   for (;;)
   {
      auto [ec, socket] = co_await acceptor.async_accept(as_tuple(deferred));
      if (ec)
      {
         if (ec == boost::system::errc::operation_canceled)
            logi("TCP accept: {}", ec.message());
         else
            logw("TCP accept: {}", ec.message());
         break;
      }

      auto ep = normalize(socket.remote_endpoint());

      //
      // Without something like a "nursery" or "async_scope", spawning a task detaches it from
      // the owning class without any means to join it. Here, we use a simple session counter to
      // track their lifetime.
      //
      {
         auto lock = std::lock_guard(m_sessionMutex);
         ++sessionCounter;
      }

      //
      // Put each connection on a strand if needed.
      //
      // NOTE: This is slow. Consider multiple IO contexts instead,
      //       or explicit thread pools where really needed.
      //
      co_spawn(config().use_strand ? boost::asio::make_strand(executor) : executor,
               handle_connection(std::move(socket)), [&, ep](const std::exception_ptr& ex) mutable
      {
         auto lock = std::lock_guard(m_sessionMutex);
         --sessionCounter;
         if (ex)
            logw("[{}] {}", ep, what(ex));
         else
            logi("[{}] session finished, {} sessions left", ep, sessionCounter);
      });
   }

   //
   // FIXME: implement a better waiting mechanism using async promises or just a condition variable.
   //
   auto lock = std::unique_lock(m_sessionMutex);
   const auto waitingFor = sessionCounter;
   logi("accept terminated, waiting for {} sessions...", waitingFor);

   size_t i = 0;
   for (; sessionCounter; ++i)
   {
      for (auto& session : m_sessions)
         session->destroy();
      m_sessions.clear();

      lock.unlock();
      co_await post(executor);
      lock.lock();
   }

   logi("accept terminated, waiting for {} sessions... done, {} iterations", waitingFor, i);
}

// =================================================================================================

} // namespace anyhttp::server
