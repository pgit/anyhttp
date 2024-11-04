#pragma once
#include "server.hpp"
#include "session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>

#include <memory>
#include <set>

namespace anyhttp
{
class Session;
}

namespace anyhttp::server
{

// =================================================================================================

class Request::Impl : public impl::Reader
{
public:
   Impl() noexcept;
   virtual ~Impl();

   // virtual const asio::any_io_executor& executor() const = 0;
   // virtual std::optional<size_t> content_length() const noexcept = 0;
   // virtual void async_read_some(ReadSomeHandler&& handler) = 0;
   // virtual void detach() = 0;
   // virtual void destroy(std::unique_ptr<Impl> self) { /* delete self */ }

   virtual boost::url_view url() const = 0;

   using ReaderOrWriter = impl::Reader;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public impl::Writer
{
public:
   Impl() noexcept;
   virtual ~Impl();

   // virtual const asio::any_io_executor& executor() const = 0;
   // virtual void content_length(std::optional<size_t> content_length) = 0;
   // virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer) = 0;
   // virtual void detach() = 0;
   // virtual void destroy(std::unique_ptr<Impl> self) { /* delete self */ }

   virtual void async_submit(WriteHandler&& handler, unsigned int status_code, Fields fields) = 0;

   using ReaderOrWriter = impl::Writer;
};

// =================================================================================================

class Server::Impl : public std::enable_shared_from_this<Server::Impl>
{
public:
   Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   const Config& config() const { return m_config; }
   const boost::asio::any_io_executor& executor() const { return m_executor; }

   asio::awaitable<void> listen_loop();
   asio::awaitable<void> handleConnection(asio::ip::tcp::socket socket);

   void listen();
   asio::ip::tcp::endpoint local_endpoint() const { return m_acceptor.local_endpoint(); }

   void setRequestHandler(RequestHandler&& handler) { m_requestHandler = std::move(handler); }
   void setRequestHandler(RequestHandlerCoro&& handler)
   {
      m_requestHandlerCoro = std::move(handler);
   }
   const RequestHandler& requestHandler() const { return m_requestHandler; }
   const RequestHandlerCoro& requestHandlerCoro() const { return m_requestHandlerCoro; }

   void run(); // FIXME: rename to spawn or async_run
   void destroy();

private:
   Config m_config;
   boost::asio::any_io_executor m_executor;
   asio::ip::tcp::acceptor m_acceptor;
   // std::unordered_map<asio::ip::tcp::endpoint, asio::ip::tcp::socket> m_sockets;
   // std::set<std::shared_ptr<Session>> m_sessions;
   std::set<std::shared_ptr<Session::Impl>> m_sessions;
   RequestHandler m_requestHandler;
   RequestHandlerCoro m_requestHandlerCoro;
   bool m_stopped = false;
};

// =================================================================================================

} // namespace anyhttp::server