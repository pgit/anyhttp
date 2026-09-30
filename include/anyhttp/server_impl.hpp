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

   /// async_submit() as a coroutine, see Reader::Impl::read_some().
   virtual Task<std::tuple<error_code>> submit(unsigned int status_code, Fields fields)
   {
      co_return co_await initiate<Status>(
         [&](StatusHandler handler) { async_submit(std::move(handler), status_code, fields); });
   }
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
   Impl(Executor executor, Config config);
   ~Impl();

   /// For log lines that belong to no connection, see anyhttp::log_prefix().
   std::string log_prefix() const { return anyhttp::log_prefix(Role::server); }

   void start();
   void destroy();

   void listen_tcp();

   const Config& config() const { return config_; }
   Executor get_executor() const noexcept { return executor_; }

   //
   // The TLS context used for every TCP connection, see make_tls_server_context().
   //
   boost::asio::ssl::context& tls_context() noexcept { return tls_context_; }

   //
   // The "Alt-Svc" field value pointing at this server's HTTP/3 endpoint, put into every response
   // sent over HTTP/1.1 and HTTP/2, see Config::alt_svc_max_age. Empty when there is nothing to
   // advertise, which is also what HTTP/3 sessions see -- they are already there.
   //
   const std::string& alt_svc() const noexcept { return alt_svc_; }

   Task<void> tcp_accept_loop();
   Task<void> handle_connection(asio::ip::tcp::socket socket);

   asio::ip::tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

   void on_request(RequestHandler&& handler) noexcept { request_handler_ = std::move(handler); }
   const RequestHandler& request_handler() const noexcept { return request_handler_; }

   //
   // Session registry, shared by all three protocols: every session is destroyed from here when
   // the server goes away. Adding fails once destroy() has swept the registry -- a session
   // registered after that sweep would never be torn down -- and the caller has to destroy the
   // session itself.
   //
   [[nodiscard]] bool add_session(std::shared_ptr<Session::Impl> session);
   void remove_session(const std::shared_ptr<Session::Impl>& session);

private:
   Config config_;

   Executor executor_;
   asio::ssl::context tls_context_;
   asio::ip::tcp::acceptor acceptor_;
   std::string alt_svc_;

   std::mutex session_mutex_;
   std::set<std::shared_ptr<Session::Impl>> sessions_;

   std::shared_ptr<Http3Server> http3_;

   RequestHandler request_handler_;
   bool destroyed_ = false;
};

// =================================================================================================

} // namespace anyhttp::server