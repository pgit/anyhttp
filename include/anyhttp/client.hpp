#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <boost/beast/core/stream_traits.hpp>

#include <boost/url.hpp>

#ifdef HAVE_CAPY
#include <boost/corosio/endpoint.hpp>
#endif

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

   using executor_type = Executor;
   executor_type get_executor() const noexcept;

public:
   int status_code() const noexcept;

public:
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

#if defined HAVE_CAPY
   struct read_some_awaitable
   {
      Response& response_;
      asio::mutable_buffer buffer_;
      mutable CapyAwaitableState<size_t> state_;

      read_some_awaitable(Response& response, asio::mutable_buffer buffer) noexcept
         : response_(response), buffer_(buffer)
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_.env = env;
         state_.continuation = {h};

         response_.async_read_some_any(buffer_, [this](boost::system::error_code ec, size_t n) mutable {
            state_.ec = ec;
            state_.result = n;
            state_.env->executor.dispatch(state_.continuation);
         });

         return std::noop_coroutine();
      }

      capy::io_result<size_t> await_resume() noexcept { return state_.resume_result(); }
   };

   auto async_read_some_capy(asio::mutable_buffer buffer)
   {
      return read_some_awaitable(*this, buffer);
   }
#endif

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

   using executor_type = Executor;
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

#if defined HAVE_CAPY
   struct get_response_awaitable
   {
      Request& request_;
      mutable CapyAwaitableState<Response> state_;

      explicit get_response_awaitable(Request& request) : request_(request) {}

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_.env = env;
         state_.continuation = {h};

         request_.async_get_response_any([this](boost::system::error_code ec, Response response) mutable {
            state_.ec = ec;
            state_.result = std::move(response);
            state_.env->executor.dispatch(state_.continuation);
         });

         return std::noop_coroutine();
      }

      capy::io_result<Response> await_resume() noexcept { return state_.resume_result(); }
   };

   auto async_get_response_capy() { return get_response_awaitable(*this); }
#endif

public:
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      // FIXME: get_executor() breaks testcase SpawnAndForget because the impl is already gone there
      auto executor = asio::get_associated_executor(token); // , get_executor());
      return asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(executor, [this](auto&& handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer);
         }),
         token, buffer);
   }

#if defined HAVE_CAPY
   struct write_awaitable
   {
      Request& request_;
      asio::const_buffer buffer_;
      mutable CapyAwaitableStateVoid state_;

      write_awaitable(Request& request, asio::const_buffer buffer) : request_(request), buffer_(buffer)
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_.env = env;
         state_.continuation = {h};

         request_.async_write_any([this](boost::system::error_code ec) mutable {
            state_.ec = ec;
            state_.env->executor.dispatch(state_.continuation);
         },
                                  buffer_);

         return std::noop_coroutine();
      }

      capy::io_result<> await_resume() noexcept { return state_.resume_result(); }
   };

   auto async_write_capy(asio::const_buffer buffer) { return write_awaitable(*this, buffer); }
#endif

private:
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer);
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
   Client(Executor executor, Config config);
   Client(Client&& other) noexcept;
   Client& operator=(Client&& other) noexcept;
   ~Client();

   using executor_type = Executor;
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

#if defined HAVE_CAPY
   /**
    * Capy-based async connect that yields io_result<Session>.
    * Usage: auto [ec, session] = co_await client.async_connect_capy(endpoint);
    */
   auto async_connect_capy(boost::corosio::endpoint ep);
#endif

private:
   void async_connect_any(ConnectHandler&& handler);
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace client
} // namespace anyhttp
