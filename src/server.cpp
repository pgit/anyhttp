
#include "anyhttp/server.hpp"
#include "anyhttp/detect_http2.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/stream.hpp"

#include <boost/asio.hpp>

using namespace std::chrono_literals;

using namespace boost::asio;

using tcp = asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using asio::awaitable;
using asio::co_spawn;

namespace anyhttp::server
{

class Server::Impl
{
public:
   explicit Impl(boost::asio::any_io_executor executor, Config config)
      : m_executor(std::move(executor)), m_config(std::move(config))
   {
      fmtlog::setLogLevel(fmtlog::LogLevel::INF);
      fmtlog::flushOn(fmtlog::LogLevel::DBG);
      run();
   }

   const boost::asio::any_io_executor& executor() const { return m_executor; }
   const Config& config() const { return m_config; }

   awaitable<void> log_loop();
   awaitable<void> listener();
   static awaitable<void> handleConnection(tcp::socket socket);

   void run()
   {
      co_spawn(m_executor, log_loop(), detached);
      co_spawn(m_executor, listener(), detached);
   }

private:
   boost::asio::any_io_executor m_executor;
   Config m_config;
};

awaitable<void> Server::Impl::log_loop()
{
   auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer(executor);
   for (;;)
   {
      fmtlog::poll();
      timer.expires_after(10ms);
      co_await timer.async_wait(deferred);
   }
}

awaitable<void> Server::Impl::handleConnection(tcp::socket socket)
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

   auto session = std::make_shared<Session>(executor, std::move(socket));
   co_spawn(executor, session->do_session(std::move(data)), [session](const std::exception_ptr&) {});
}


awaitable<void> Server::Impl::listener()
{
   auto executor = co_await boost::asio::this_coro::executor;
   tcp::acceptor acceptor(executor);

   boost::system::error_code ec;
   ip::address address = ip::address::from_string(config().listen_address, ec);
   if (ec)
      logw("server: error resolving '{}': {}", config().listen_address, ec.what());

   tcp::endpoint endpoint(address, config().port);
   if (endpoint.protocol() == tcp::v6())
      std::ignore = acceptor.set_option(ip::v6_only(false), ec);

   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(endpoint);
   acceptor.listen();

   endpoint = acceptor.local_endpoint();
   logi("server: listening on {}", endpoint);

   for (;;)
   {
      auto socket = co_await acceptor.async_accept(deferred);
      co_spawn(executor, handleConnection(std::move(socket)), [](const std::exception_ptr&) {});
   }
}

Server::Server(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
}
Server::~Server() = default;

} // namespace anyhttp::server
