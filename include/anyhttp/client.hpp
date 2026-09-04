#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <boost/beast/core/stream_traits.hpp>

#include <boost/url.hpp>

namespace anyhttp
{
class Session;
namespace client
{

// =================================================================================================

struct Config
{
   // FIXME: the client does not connect to an URL, it connects to a host:port or endpoint
   boost::urls::url url{"localhost:8080"};
   Protocol protocol{Protocol::h2};
};

// =================================================================================================

class Response
{
public:
   class Impl;
   Response();
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   void reset() noexcept;
   ~Response();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

public:
   int status_code() const noexcept;

public:
   /**
    * Reads a part of the response body.
    *
    * The end of the body is reported as \c asio::error::eof with zero bytes, as ASIO does
    * everywhere else, and so is every read after it. A body cut short by a reset stream or a lost
    * connection completes with \c http::error::partial_message instead.
    */
   template <typename Buffers,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
      requires(boost::asio::is_mutable_buffer_sequence<Buffers>::value)
   auto async_read_some(const Buffers& buffers, CompletionToken&& token = CompletionToken())
   {
      for (auto& buffer : buffers)
         if (buffer.size() > 0)
            return async_read_some(buffer, std::forward<CompletionToken>(token));
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
   auto async_read_some(asio::mutable_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, ReadSome>(
         [&](ReadSomeHandler handler, asio::mutable_buffer buffer) { //
            async_read_some_any(buffer, std::move(handler));
         },
         token, buffer);
   }

private:
   void async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler);
   std::shared_ptr<Impl> impl;
};

static_assert(boost::beast::is_async_read_stream<Response>::value);

// -------------------------------------------------------------------------------------------------

class Request
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   void reset() noexcept;
   ~Request();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

public:
   using GetResponse = void(boost::system::error_code, Response);
   using GetResponseHandler = asio::any_completion_handler<GetResponse>;

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(GetResponse) CompletionToken = DefaultCompletionToken>
   auto async_get_response(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, GetResponse>(
         asio::bind_executor(executor, [this](auto&& handler) { //
            async_get_response_any(std::move(handler));
         }),
         token);
   }

public:
   /**
    * Writes \p buffer as part of the request body, which stays open for more.
    *
    * An empty buffer writes nothing and completes immediately -- use \c async_write_eof() to end
    * the body.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      // FIXME: get_executor() breaks testcase SpawnAndForget because the impl is already gone there
      auto executor = asio::get_associated_executor(token); // , get_executor());
      return asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(executor, [this](auto&& handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer, false);
         }),
         token, buffer);
   }

   /**
    * Writes \p buffer as the last part of the request body and ends it.
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
      // see async_write() above for why the executor is not defaulted to get_executor()
      auto executor = asio::get_associated_executor(token);
      return asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(executor, [this](auto&& handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer, true);
         }),
         token, buffer);
   }

   /// Ends the request body without writing anything more.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(CompletionToken&& token = CompletionToken())
   {
      return async_write_eof(asio::const_buffer{}, std::forward<CompletionToken>(token));
   }

private:
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof);
   void async_get_response_any(GetResponseHandler&& handler);
   std::shared_ptr<Impl> impl;
};

// static_assert(boost::beast::is_async_write_stream<Request>::value);

// =================================================================================================

using Connect = void(boost::system::error_code, Session);
using ConnectHandler = asio::any_completion_handler<Connect>;

class Client
{
public:
   class Impl;
   Client(asio::any_io_executor executor, Config config);
   Client(Client&& other) noexcept;
   Client& operator=(Client&& other) noexcept;
   ~Client();

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   /**
    * Connect to configured peer and establish a new session.
    *
    * This operation supports 'terminal' cancellation. When cancellation is requested, it may
    * take some time to be executed. This is because the async resolver eventually calls
    * \c getaddrinfo(), which is a blocking system call. This is done from a separate thread,
    * so the users executor is not blocked, but this still means that the operation cannot be
    * interrupted.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken = DefaultCompletionToken>
   auto async_connect(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Connect>(
         bind_executor(executor, [&](auto&& handler) { //
            async_connect_any(std::move(handler));
         }),
         token);
   }

private:
   void async_connect_any(ConnectHandler&& handler);
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace client
} // namespace anyhttp
