#pragma once

#include "common.hpp" // IWYU pragma: keep
#include "reader.hpp"
#include "writer.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include <boost/url/urls.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

using namespace std::chrono_literals;

namespace anyhttp::server
{

// =================================================================================================

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
   bool use_strand = false;

   //
   // The largest header section of a request the server accepts, in bytes. A request with more is
   // answered with 431 (Request Header Fields Too Large) and never reaches the request handler.
   // HTTP/1.1 counts the request line and header lines as received, and closes the connection
   // after the 431 response. HTTP/2 and HTTP/3 count each field as its name and value plus 32
   // bytes (pseudo-headers included) and announce the limit to the client in their SETTINGS.
   //
   // Fields beyond the limit are not stored, so this bounds the memory a request can take up with
   // its headers. A single field is also limited by the protocol libraries: 64 KiB for HTTP/2 and
   // HTTP/3, where a larger one fails the whole connection. So does an HTTP/2 header block that
   // takes up more CONTINUATION frames than a header section of this size needs (but at least 8).
   //
   size_t max_header_size = default_max_header_size;

   //
   // How long a client may remember the HTTP/3 endpoint this server advertises in every response
   // it sends over HTTP/1.1 and HTTP/2, as "Alt-Svc: h3=\":<port>\"; ma=<seconds>" (RFC 7838).
   // A client that takes it up makes its *next* connection over QUIC -- there is no in-band
   // upgrade to HTTP/3, so this is the whole of it. HTTP/3 shares the endpoint the TCP acceptor
   // is listening on, so what is advertised is the port and nothing else: the same host, over
   // QUIC. Zero sends no "Alt-Svc" at all.
   //
   // Advertising over cleartext HTTP is of no use to browsers and curl, which honour "Alt-Svc"
   // for https:// origins only -- the alternative has to be at least as secure as the origin.
   //
   std::chrono::seconds alt_svc_max_age = 24h;

   //
   // HTTP/3 only: how long a QUIC connection may go without a packet from its peer before it is
   // dropped. This is the only way a peer that vanished without a CONNECTION_CLOSE -- a killed
   // client, a machine that went to sleep -- is ever noticed, so it also bounds how long its
   // session and streams stay around. 30s is what the ngtcp2 examples use.
   //
   std::chrono::nanoseconds idle_timeout = 30s;

   //
   // HTTP/3 only, testing aid: probability (0.0 ... 1.0) with which an individual QUIC datagram
   // is thrown away instead of being processed (rx) or actually sent (tx). This exercises loss
   // recovery -- retransmits, PTO, ACK handling -- without needing a lossy network. Dropping
   // happens per QUIC packet, i.e. GRO-coalesced datagrams are dropped individually and TX
   // GSO batching is bypassed while `drop_rate_tx` is non-zero.
   //
   double drop_rate_rx = 0.0;
   double drop_rate_tx = 0.0;

   //
   // HTTP/3 only, for benchmarking: turn off the kernel's UDP offloads. Without GRO, every received
   // datagram costs a recvmsg() of its own; without GSO, every QUIC packet costs a sendto() of its
   // own instead of a whole same-sized run going out in one sendmsg().
   //
   bool disable_gro = false;
   bool disable_gso = false;
};

// =================================================================================================

class Request : public Reader
{
public:
   class Impl;
   explicit Request(std::shared_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   void reset() noexcept;
   ~Request();

public:
   boost::url_view url() const;

   /// The request header fields, without HTTP/2 and HTTP/3 pseudo-headers.
   const Fields& fields() const;

   /**
    * Looks up a query parameter and converts its value to \c T.
    *
    * Returns \c std::nullopt if the parameter is missing, has no value at all, or if its value
    * does not convert to \c T -- the latter is logged as a warning. Use \c value_or() for a
    * default:
    *
    * \code
    * auto delay = request.get_param_as<size_t>("delay").value_or(0);
    * \endcode
    */
   template <typename T>
   std::optional<T> get_param_as(std::string_view name) const
   {
      const auto u = url(); // keep the url_view alive: params() only references it
      const auto params = u.params();
      const auto it = params.find(name);
      if (it == params.end() || !(*it).has_value)
         return std::nullopt;

      const std::string& value = (*it).value;

      //
      // lexical_cast wraps a negative number around into an unsigned type -- "-1" arrives as
      // SIZE_MAX -- which is never what a caller asking for an unsigned type wants.
      //
      if (std::is_integral_v<T> && std::is_unsigned_v<T> && value.starts_with('-'))
         ; // invalid value (reported below)
      else if (T converted; boost::conversion::try_lexical_convert(value, converted))
         return converted;

      logw("get_param_as: invalid value '{}' for parameter '{}'", value, name);
      return std::nullopt;
   }

private:
   //
   // Hides Reader::pimpl(), narrowing it to the implementation this handle was built from.
   //
   Impl& pimpl() const noexcept;
};

// -------------------------------------------------------------------------------------------------

template <typename T>
awaitable<void> sleep(T duration)
{
   using namespace asio;
   steady_timer timer(co_await this_coro::executor);
   timer.expires_after(duration);
   co_await timer.async_wait();
}

class Response : public Writer
{
public:
   class Impl;
   explicit Response(std::shared_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   void reset() noexcept;
   ~Response();

public:
   /**
    * Sends the response header, which opens the body for writing.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Status) CompletionToken = DefaultCompletionToken>
   auto async_submit(unsigned int status_code, const Fields& headers,
                     CompletionToken&& token = CompletionToken())
   {
      // binding the executor lets tokens that need one -- cancel_after's timer -- find it here
      return asio::async_initiate<CompletionToken, Status>(
         asio::bind_executor(asio::get_associated_executor(token, get_executor()),
                             [this](StatusHandler handler, unsigned int status_code,
                                    const Fields& headers) { //
                                async_submit_any(std::move(handler), status_code, headers);
                             }),
         token, status_code, headers);
   }

private:
   void async_submit_any(StatusHandler&& handler, unsigned int status_code, const Fields& headers);

   /// Hides Writer::pimpl(), narrowing it to the implementation this handle was built from.
   Impl& pimpl() const noexcept;
};

// =================================================================================================

using RequestHandler = std::function<asio::awaitable<void>(Request, Response)>;

class Server
{
public:
   class Impl;
   Server(asio::any_io_executor executor, Config config);
   Server(Server&& other) noexcept;
   Server& operator=(Server&& other) noexcept;
   ~Server();

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   void setRequestHandler(RequestHandler&& handler);

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server
