#pragma once
#include "server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>

#include <set>

#include <spdlog/spdlog.h>

namespace anyhttp::server
{

// =================================================================================================

class Stream;
class Request::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual const asio::any_io_executor& executor() const = 0;
   virtual void async_read_some(ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual const asio::any_io_executor& executor() const = 0;
   virtual void write_head(unsigned int status_code, Headers headers) = 0;
   virtual void async_write(WriteHandler&& handler, std::vector<uint8_t> bufffer) = 0;
   virtual void detach() = 0;
};

// =================================================================================================

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
   void setRequestHandler(RequestHandlerCoro&& handler)
   {
      m_requestHandlerCoro = std::move(handler);
   }
   const RequestHandler& requestHandler() const { return m_requestHandler; }
   const RequestHandlerCoro& requestHandlerCoro() const { return m_requestHandlerCoro; }

   void run();

private:
   Config m_config;
   boost::asio::any_io_executor m_executor;
   ip::tcp::acceptor m_acceptor;
   std::set<std::shared_ptr<Session>> m_sessions;
   RequestHandler m_requestHandler;
   RequestHandlerCoro m_requestHandlerCoro;
};

// =================================================================================================

} // namespace anyhttp::server
