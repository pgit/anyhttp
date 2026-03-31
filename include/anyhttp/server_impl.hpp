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

   // FIXME: doesn't make sense to have a status_code() for a server request, but keeps beast happy
   virtual unsigned int status_code() const noexcept = 0;
   virtual boost::url_view url() const = 0;

   using ReaderOrWriter = impl::Reader;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public impl::Writer
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual void async_submit(WriteHandler&& handler, unsigned int status_code,
                             const Fields& fields) = 0;

   using ReaderOrWriter = impl::Writer;
};

// =================================================================================================

struct Endpoint;

class Server::Impl : public std::enable_shared_from_this<Server::Impl>
{
public:
   Impl(Executor executor, Config config);
   ~Impl();

   void start();
   void destroy();

   void listen_tcp();
   void listen_udp();

   const Config& config() const { return m_config; }
   Executor get_executor() const noexcept { return m_executor; }

   Awaitable<void> listen_loop();
   Awaitable<void> handleConnection(asio::ip::tcp::socket socket);

   asio::ip::tcp::endpoint local_endpoint() const
   {
      assert(m_acceptor);
      return m_acceptor->local_endpoint();
   }

   void setRequestHandler(RequestHandler&& handler) { m_requestHandler = std::move(handler); }
   void setRequestHandler(RequestHandlerCoro&& handler)
   {
      m_requestHandlerCoro = std::move(handler);
   }
   const RequestHandler& requestHandler() const { return m_requestHandler; }
   const RequestHandlerCoro& requestHandlerCoro() const { return m_requestHandlerCoro; }

   Awaitable<void> udp_receive_loop();
   int udp_on_read(Endpoint& ep);

private:
   Config m_config;

   Executor m_executor;
   std::optional<TcpAcceptor> m_acceptor;
   std::optional<UdpSocket> m_udp_socket;

   std::mutex m_sessionMutex;
   std::set<std::shared_ptr<Session::Impl>> m_sessions;

   RequestHandler m_requestHandler;
   RequestHandlerCoro m_requestHandlerCoro;
   bool m_stopped = false;
};

// =================================================================================================

} // namespace anyhttp::server