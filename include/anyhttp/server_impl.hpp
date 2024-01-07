#pragma once
#include "server.hpp"

#include <set>

#include <boost/asio.hpp>

#include <spdlog/spdlog.h>

using namespace boost::asio;

namespace anyhttp::server
{

class Request::Impl
{
};

class Response::Impl
{
};

class Session;
class Server::Impl
{
public:
   explicit Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   const Config& config() const { return m_config; }
   const boost::asio::any_io_executor& executor() const { return m_executor; }

   awaitable<void> listen_loop();
   awaitable<void> handleConnection(ip::tcp::socket socket);

   void listen();
   ip::tcp::endpoint local_endpoint() const { return m_acceptor.local_endpoint(); }

   void setRequestHandler(RequestHandler&& handler) { m_requestHandler = std::move(handler); }
   const RequestHandler& requestHandler() const { return m_requestHandler; }

   void run();

private:
   Config m_config;
   boost::asio::any_io_executor m_executor;
   ip::tcp::acceptor m_acceptor;
   std::set<std::shared_ptr<Session>> m_sessions;
   RequestHandler m_requestHandler;
};

} // namespace anyhttp::server
