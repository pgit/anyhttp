#pragma once
#include "client.hpp"
#include "reader_impl.hpp"
#include "writer_impl.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/experimental/co_composed.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace anyhttp::client
{

// =================================================================================================

class Request::Impl : public Writer::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual void async_submit(StatusHandler&& handler, unsigned int status_code,
                             const Fields& headers) = 0;
   virtual void async_get_response(GetResponseHandler&& handler) = 0;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public Reader::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   //
   // The status line, as it arrived. A response has no method or URL -- those are the other half
   // of the exchange, on server::Request::Impl.
   //
   virtual unsigned int status_code() const noexcept = 0;
   virtual const Fields& fields() const = 0;
};

// =================================================================================================

class Client::Impl
{
public:
   explicit Impl(asio::any_io_executor executor, Config config);
   ~Impl();

   boost::asio::any_io_executor get_executor() const noexcept { return m_executor; }

   void async_connect(ConnectHandler handler);
   const Config& config() const { return m_config; }

   // ----------------------------------------------------------------------------------------------

   /**
    * Where the origin is also reachable over HTTP/3, as a server has advertised it (RFC 7838).
    *
    * There is room for exactly one of these, not a cache keyed by origin: a Client connects to
    * the one authority its Config::url names, so every response it ever sees comes from that
    * same origin.
    */
   struct AlternativeService
   {
      std::string host; ///< empty if the alternative is on the host of the origin itself
      std::string port;
      std::chrono::steady_clock::time_point expires;
   };

   /**
    * Takes in an "Alt-Svc" field value seen by a session of this client -- a header field on a
    * response, or the payload of an HTTP/2 ALTSVC frame, which have the same syntax.
    *
    * Only an "h3" alternative is remembered, and only with Config::follow_alt_svc set: HTTP/3 is
    * the one thing anyhttp can move a *later* connection to, while a client already told to speak
    * HTTP/1.1 or HTTP/2 has no use for an alternative offering those.
    */
   void on_alt_svc(std::string_view field_value);

   /// The advertised HTTP/3 endpoint, as long as its "ma" has not run out.
   std::optional<AlternativeService> alt_svc() const;

private:
   awaitable<Session> async_connect();

private:
   Config m_config;
   asio::any_io_executor m_executor;
   std::optional<asio::ip::tcp::resolver> m_resolver;

   //
   // Sessions run on their own executor, which is not necessarily the one the next connect is
   // made from, so this is reached from more than one thread.
   //
   mutable std::mutex m_altSvcMutex;
   std::optional<AlternativeService> m_altSvc;
};

// =================================================================================================

} // namespace anyhttp::client
