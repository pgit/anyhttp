#include "anyhttp/server_impl.hpp"
#include "anyhttp/beast_session.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/detect_http2.hpp"

#include "anyhttp/beast_session.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core/flat_buffer.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::server
{

// =================================================================================================

Request::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Request: ctor\x1b[0m"); }
Request::Impl::~Impl() { logd("\x1b[35mServer::Request: dtor\x1b[0m"); }

Response::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Response: ctor\x1b[0m"); }
Response::Impl::~Impl() { logd("\x1b[35mServer::Response: dtor\x1b[0m"); }

// =================================================================================================

Server::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
#if !defined(NDEBUG)
   spdlog::set_level(spdlog::level::debug);
#else
   spdlog::set_level(spdlog::level::info);
#endif
   spdlog::info("Server: ctor");
   listen();
   // run();
}

Server::Impl::~Impl()
{
   logi("Server: dtor");
   assert(m_stopped);
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::run()
{
   // co_spawn(m_executor, listen_loop(), detached);
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
   m_stopped = true;
   m_acceptor.close();
}

awaitable<void> Server::Impl::handleConnection(ip::tcp::socket socket)
{
   logi("[{}] new connection", normalize(socket.remote_endpoint()));
   auto executor = co_await boost::asio::this_coro::executor;

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

   //
   // detect HTTP2 client preface, fallback to HTTP/1.1 if not found
   //
   std::vector<uint8_t> data;
   auto buffer = boost::asio::dynamic_buffer(data);
   if (co_await async_detect_http2_client_preface(socket, buffer, deferred))
   {
      logi("[{}] detected HTTP2 client preface, {} bytes in buffer",
           normalize(socket.remote_endpoint()), buffer.size());
      auto session = std::make_shared<nghttp2::NGHttp2Session>(*this, executor, std::move(socket));
      co_await session->do_server_session(std::move(data));
   }
   else
   {
      logi("[{}] no HTTP2 client preface, assuming HTTP/1.x", normalize(socket.remote_endpoint()));
      auto session = std::make_shared<beast_impl::BeastSession>(*this, executor, std::move(socket));
      co_await session->do_server_session(std::move(data));
   }
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::listen()
{
   // tcp::acceptor acceptor(executor());
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

awaitable<void> Server::Impl::listen_loop()
{
   auto executor = co_await boost::asio::this_coro::executor;

   for (;;)
   {
      auto [ec, socket] = co_await m_acceptor.async_accept(as_tuple(deferred));
      if (ec)
      {
         logw("accept: {}", ec.message());
         break;
      }

      co_spawn(
         executor, [&]() { return handleConnection(std::move(socket)); },
         [](const std::exception_ptr& ex)
         {
            if (ex)
               logw("listen_loop: {}", what(ex));
         });
   }
}

// =================================================================================================

} // namespace anyhttp::server
