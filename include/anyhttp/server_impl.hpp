#pragma once
#include "reader_impl.hpp"
#include "server.hpp"
#include "session.hpp"
#include "writer_impl.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include <memory>
#include <set>

namespace anyhttp
{
class Session;
}

namespace anyhttp::server
{

// =================================================================================================

class Request::Impl : public Reader::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   //
   // The request line, as it arrived. A request has no status code -- that is the other half of
   // the exchange, on client::Response::Impl.
   //
   virtual std::string_view method() const noexcept = 0;
   virtual boost::url_view url() const = 0;
   virtual const Fields& fields() const = 0;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public Writer::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual void async_submit(StatusHandler&& handler, unsigned int status_code,
                             const Fields& fields) = 0;
};

// =================================================================================================

//
// The HTTP/3 half of the server, behind anyhttp/h3_backend.hpp: it owns the UDP socket and
// everything QUIC, so that nothing of ngtcp2/nghttp3 reaches this header.
//
class Http3Server;

class Server::Impl : public std::enable_shared_from_this<Server::Impl>
{
public:
   Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   void start();
   void destroy();

   void listen_tcp();

   const Config& config() const { return m_config; }
   boost::asio::any_io_executor get_executor() const noexcept { return m_executor; }

   //
   // The TLS context used for every TCP connection, see make_tls_server_context().
   //
   boost::asio::ssl::context& tls_context() noexcept { return m_tlsContext; }

   //
   // The "Alt-Svc" field value pointing at this server's HTTP/3 endpoint, put into every response
   // sent over HTTP/1.1 and HTTP/2, see Config::alt_svc_max_age. Empty when there is nothing to
   // advertise, which is also what HTTP/3 sessions see -- they are already there.
   //
   const std::string& alt_svc() const noexcept { return m_altSvc; }

   asio::awaitable<void> tcp_accept_loop();
   asio::awaitable<void> handle_connection(asio::ip::tcp::socket socket);

   asio::ip::tcp::endpoint local_endpoint() const
   {
      assert(m_acceptor);
      return m_acceptor->local_endpoint();
   }

   void setRequestHandler(RequestHandler&& handler) { m_requestHandler = std::move(handler); }
   const RequestHandler& requestHandler() const { return m_requestHandler; }

   //
   // Session registry, shared by all three protocols: every session is destroyed from here when
   // the server goes away. Adding fails once destroy() has swept the registry -- a session
   // registered after that sweep would never be torn down -- and the caller has to destroy the
   // session itself.
   //
   [[nodiscard]] bool add_session(std::shared_ptr<Session::Impl> session);
   void remove_session(const std::shared_ptr<Session::Impl>& session);

private:
   Config m_config;

   boost::asio::any_io_executor m_executor;
   boost::asio::ssl::context m_tlsContext;
   std::optional<asio::ip::tcp::acceptor> m_acceptor;
   std::string m_altSvc;

   std::mutex m_sessionMutex;
   std::set<std::shared_ptr<Session::Impl>> m_sessions;

   std::shared_ptr<Http3Server> m_http3;

   RequestHandler m_requestHandler;
   bool m_destroyed = false;
};

// =================================================================================================

} // namespace anyhttp::server