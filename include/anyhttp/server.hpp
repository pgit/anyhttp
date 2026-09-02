#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include <boost/url/urls.hpp>

#include <optional>
#include <string_view>

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
};

// =================================================================================================

class Request
{
public:
   class Impl;
   explicit Request(std::shared_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   void reset() noexcept;
   ~Request();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   boost::url_view url() const;
   std::optional<size_t> content_length() const noexcept;

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
      const auto params = url().params();
      const auto it = params.find(name);
      if (it == params.end() || !(*it).has_value)
         return std::nullopt;

      if (T value; boost::conversion::try_lexical_convert((*it).value, value))
         return value;

      logw("get_param_as: invalid value '{}' for parameter '{}'", (*it).value, name);
      return std::nullopt;
   }

public:
   /**
    * Reads a part of the request body.
    *
    * The end of the body is reported as \c asio::error::eof with zero bytes, as ASIO does
    * everywhere else, and so is every read after it. A body cut short by a reset stream or a lost
    * connection completes with \c http::error::partial_message instead.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
   auto async_read_some(boost::asio::mutable_buffer buffer,
                        CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, ReadSome>(
         [&](ReadSomeHandler handler, asio::mutable_buffer buffer) { //
            async_read_some_any(buffer, std::move(handler));
         },
         token, buffer);
   }

private:
   void async_read_some_any(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler);
   std::shared_ptr<Impl> impl;
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

class Response
{
public:
   class Impl;
   explicit Response(std::shared_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   void reset() noexcept;
   ~Response();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   void content_length(std::optional<size_t> content_length);

public:
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Status) CompletionToken = DefaultCompletionToken>
   auto async_submit(unsigned int status_code, const Fields& headers,
                     CompletionToken&& token = CompletionToken())
   {
      // binding the executor lets tokens that need one -- cancel_after's timer -- find it here
      return boost::asio::async_initiate<CompletionToken, Status>(
         asio::bind_executor(get_executor(),
                             [this](StatusHandler handler, unsigned int status_code,
                                    const Fields& headers) { //
                                async_submit_any(std::move(handler), status_code, headers);
                             }),
         token, status_code, headers);
   }

   /**
    * Writes \p buffer as part of the response body, which stays open for more.
    *
    * An empty buffer writes nothing and completes immediately -- use \c async_write_eof() to end
    * the body.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      // binding the executor lets tokens that need one -- cancel_after's timer -- find it here
      return boost::asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(get_executor(),
                             [this](WriteHandler handler, asio::const_buffer buffer) { //
                                async_write_any(std::move(handler), buffer, false);
                             }),
         token, buffer);
   }

   /**
    * Writes \p buffer as the last part of the response body and ends it.
    *
    * Both go out together, so ending a body that has a tail of data left costs no more than
    * writing that tail: no second, empty write and no extra round trip through the protocol
    * stack. Re-ending an already-ended body with an empty buffer completes immediately and
    * changes nothing; with data attached it completes with \c errc::broken_pipe, just as writing
    * that data would -- there is no body left for it to belong to.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      // binding the executor lets tokens that need one -- cancel_after's timer -- find it here
      return boost::asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(get_executor(),
                             [this](WriteHandler handler, asio::const_buffer buffer) { //
                                async_write_any(std::move(handler), buffer, true);
                             }),
         token, buffer);
   }

   /// Ends the response body without writing anything more.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(CompletionToken&& token = CompletionToken())
   {
      return async_write_eof(asio::const_buffer{}, std::forward<CompletionToken>(token));
   }

private:
   void async_submit_any(StatusHandler&& handler, unsigned int status_code, const Fields& headers);
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof);
   std::shared_ptr<Impl> impl;
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
