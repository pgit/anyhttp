#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_composed.hpp>
#include <boost/asio/co_spawn.hpp>

#include <boost/url/urls.hpp>

using namespace std::chrono_literals;

namespace anyhttp::server
{

// =================================================================================================

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
   bool use_strand = false;
};

// =================================================================================================

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

   boost::url_view url() const;
   std::optional<size_t> content_length() const noexcept;

public:
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
   std::unique_ptr<Impl> impl;
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
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   void reset() noexcept;
   ~Response();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   void content_length(std::optional<size_t> content_length);

public:
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_submit(unsigned int status_code, const Fields& headers,
                     CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [this](WriteHandler handler, unsigned int status_code, const Fields& headers) { //
            async_submit_any(std::move(handler), status_code, headers);
         },
         token, status_code, headers);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [this](WriteHandler handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer);
         },
         token, buffer);
   }

   // https://github.com/chriskohlhoff/asio/blob/231cb29bab30f82712fcd54faaea42424cc6e710/asio/src/tests/unit/co_composed.cpp#L45
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Write>(
         asio::co_composed<Write>(
            [this](auto state, asio::const_buffer buffer,
                   asio::any_io_executor executor) mutable -> void { //
               co_await asio::co_spawn(executor, sleep(100ms), asio::deferred);
               co_await async_write(buffer);
               co_await async_write({});
               co_return {boost::system::error_code{}};
            },
            get_executor()),
         token, buffer, get_executor());
   }

private:
   void async_submit_any(WriteHandler&& handler, unsigned int status_code, const Fields& headers);
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer);
   std::unique_ptr<Impl> impl;
};

// =================================================================================================

using RequestHandler = std::function<void(Request, Response)>;
using RequestHandlerCoro = std::function<asio::awaitable<void>(Request, Response)>;

class Server
{
public:
   class Impl;
   Server(asio::any_io_executor executor, Config config);
   ~Server();

   Server(Server&& other) = default;
   Server& operator=(Server&& other) = default;

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   void setRequestHandler(RequestHandler&& handler);
   void setRequestHandlerCoro(RequestHandlerCoro&& handler);

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server
