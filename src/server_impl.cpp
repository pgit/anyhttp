#include "anyhttp/server_impl.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/detect_http2.hpp"
#include "anyhttp/stream.hpp" // IWYU pragma: keep

#include <boost/asio/error.hpp>
#include <set>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;
using asio::co_spawn;
using asio::deferred;

namespace anyhttp::server
{

// =================================================================================================

Request::Impl::Impl() noexcept = default;
Request::Impl::~Impl() = default;
Response::Impl::Impl() noexcept = default;
Response::Impl::~Impl() = default;

// =================================================================================================

Server::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
   spdlog::set_level(spdlog::level::info);
   spdlog::info("Server: ctor");
   listen();
   run();
}

Server::Impl::~Impl() { logi("Server: dtor"); }

void Server::Impl::run() { co_spawn(m_executor, listen_loop(), detached); }

awaitable<void> Server::Impl::handleConnection(ip::tcp::socket socket)
{
   auto executor = co_await boost::asio::this_coro::executor;

   //
   // detect HTTP2 client preface, abort connection if not found
   //
   std::vector<uint8_t> data;
   auto buffer = boost::asio::dynamic_buffer(data);
   if (!co_await async_detect_http2_client_preface(socket, buffer, deferred))
   {
      fmt::print("no HTTP2 client preface detected ({} bytes in buffer), closing connection\n",
                 buffer.size());

      socket.shutdown(asio::ip::tcp::socket::shutdown_send);
      socket.close();
      co_return;
   }

   auto session = std::make_shared<nghttp2::Session>(*this, executor, std::move(socket));
   // m_sessions.emplace(session);
#if 1
   co_await session->do_session(std::move(data));
   // m_sessions.erase(session);
#else
   co_spawn(executor, session->do_session(std::move(data)),
            [this, session](const std::exception_ptr&) { m_sessions.erase(session); });
#endif
}

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

awaitable<void> Server::Impl::listen_loop()
{
   auto executor = co_await boost::asio::this_coro::executor;

   for (;;)
   {
      auto [ec, socket] = co_await m_acceptor.async_accept(as_tuple(deferred));
      if (ec)
         break;

      co_spawn(
         executor, [&]() { return handleConnection(std::move(socket)); },
         [](const std::exception_ptr&) {});
   }
}

// =================================================================================================

} // namespace anyhttp::server
