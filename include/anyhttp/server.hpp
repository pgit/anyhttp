#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_composed.hpp>
#include <boost/asio/co_spawn.hpp>

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
   explicit Request(std::shared_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   void reset() noexcept;
   ~Request();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

   using executor_type = Executor;
   executor_type get_executor() const noexcept;

   boost::urls::url_view url() const;
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

#if defined HAVE_CAPY
   struct read_some_awaitable
   {
      Request& request_;
      boost::asio::mutable_buffer buffer_;
      std::shared_ptr<CapyAwaitableState<size_t>> state_;

      read_some_awaitable(Request& request, boost::asio::mutable_buffer buffers) noexcept
         : request_(request), buffer_(std::move(buffers)),
           state_(std::make_shared<CapyAwaitableState<size_t>>())
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_->env = env;
         state_->continuation = {h};

         request_.async_read_some_any(
            buffer_, [state = state_](boost::system::error_code ec, size_t bytes_transferred) mutable
         {
            state->ec = ec;
            state->result = bytes_transferred;
            state->env->executor.dispatch(state->continuation);
         });

         return std::noop_coroutine();
      }

      capy::io_result<size_t> await_resume() noexcept
      {
         return state_->resume_result();
      }
   };

   auto async_read_some_capy(boost::asio::mutable_buffer buffer)
   {
      return read_some_awaitable(*this, buffer);
   }
#endif

private:
   void async_read_some_any(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler);
   std::shared_ptr<Impl> impl;
};

// -------------------------------------------------------------------------------------------------

template <typename T>
Awaitable<void> sleep(T duration)
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

   using executor_type = Executor;
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

#if defined HAVE_CAPY
   struct submit_awaitable
   {
      Response& response_;
      unsigned int status_code_;
      Fields headers_;
      std::shared_ptr<CapyAwaitableStateVoid> state_;

      submit_awaitable(Response& response, unsigned int status_code, const Fields& headers)
         : response_(response), status_code_(status_code), headers_(headers),
           state_(std::make_shared<CapyAwaitableStateVoid>())
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_->env = env;
         state_->continuation = {h};

         response_.async_submit_any(
            [state = state_](boost::system::error_code ec) mutable {
               state->ec = ec;
               state->env->executor.dispatch(state->continuation);
            },
            status_code_, headers_);

         return std::noop_coroutine();
      }

      capy::io_result<> await_resume() noexcept { return state_->resume_result(); }
   };

   auto async_submit_capy(unsigned int status_code, const Fields& headers)
   {
      return submit_awaitable(*this, status_code, headers);
   }

   struct write_awaitable
   {
      Response& response_;
      asio::const_buffer buffer_;
      std::shared_ptr<CapyAwaitableStateVoid> state_;

      write_awaitable(Response& response, asio::const_buffer buffer)
         : response_(response), buffer_(buffer),
           state_(std::make_shared<CapyAwaitableStateVoid>())
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_->env = env;
         state_->continuation = {h};

         response_.async_write_any([state = state_](boost::system::error_code ec) mutable {
            state->ec = ec;
            state->env->executor.dispatch(state->continuation);
         },
                                  buffer_);

         return std::noop_coroutine();
      }

      capy::io_result<> await_resume() noexcept { return state_->resume_result(); }
   };

   auto async_write_capy(asio::const_buffer buffer) { return write_awaitable(*this, buffer); }

   struct write_eof_awaitable
   {
      Response& response_;
      asio::const_buffer buffer_;
      std::shared_ptr<CapyAwaitableStateVoid> state_;

      write_eof_awaitable(Response& response, asio::const_buffer buffer)
         : response_(response), buffer_(buffer),
           state_(std::make_shared<CapyAwaitableStateVoid>())
      {
      }

      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
      {
         state_->env = env;
         state_->continuation = {h};

         response_.async_write_any([state = state_](boost::system::error_code ec) mutable {
            if (ec)
            {
               state->ec = ec;
               state->env->executor.dispatch(state->continuation);
               return;
            }

            // Continue without second async_write_any for now
            // Second write should be handled as separate call if needed
            state->ec = ec;
            state->env->executor.dispatch(state->continuation);
         },
                                  buffer_);

         return std::noop_coroutine();
      }

      capy::io_result<> await_resume() noexcept { return state_->resume_result(); }
   };

   auto async_write_eof_capy(asio::const_buffer buffer)
   {
      return write_eof_awaitable(*this, buffer);
   }
#endif

   // https://github.com/chriskohlhoff/asio/blob/231cb29bab30f82712fcd54faaea42424cc6e710/asio/src/tests/unit/co_composed.cpp#L45
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Write>(
         asio::co_composed<Write>(
            [this](auto state, asio::const_buffer buffer,
                   Executor executor) mutable -> void { //
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
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

using RequestHandler = std::function<void(Request, Response)>;
using RequestHandlerCoro = std::function<Awaitable<void>(Request, Response)>;

class Server
{
public:
   class Impl;
   Server(Executor executor, Config config);
   Server(Server&& other) noexcept;
   Server& operator=(Server&& other) noexcept;
   ~Server();

   using executor_type = Executor;
   executor_type get_executor() const noexcept;

   void setRequestHandler(RequestHandler&& handler);
   void setRequestHandlerCoro(RequestHandlerCoro&& handler);

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server
