
#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

#ifdef HAVE_CAPY
#include "anyhttp/session.hpp"
#endif

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <utility>

namespace anyhttp::client
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl_) : impl(std::move(impl_))
{
   if (impl)
      logd("\x1b[1;34mClient::Request: ctor\x1b[0m");
}

Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&& other) noexcept = default;

void Request::reset() noexcept
{
   if (impl)
   {
      logd("\x1b[34mClient::Request: dtor\x1b[0m");
      impl->destroy();
      impl.reset();
   }
}

Request::~Request() { reset(); }

// -------------------------------------------------------------------------------------------------

void Request::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (impl)
      impl->async_write(std::move(handler), buffer);
   else
      std::move(handler)(boost::asio::error::bad_descriptor);
}

void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   if (impl)
      impl->async_get_response(std::move(handler));
   else
      std::move(handler)(boost::asio::error::bad_descriptor, Response{nullptr});
}

Executor Request::get_executor() const noexcept
{
   assert(impl);
   return impl->get_executor();
}

// =================================================================================================

Response::Response() : impl(nullptr) {}

Response::Response(std::unique_ptr<Response::Impl> impl_) : impl(std::move(impl_))
{
   if (impl)
      logd("\x1b[1;34mClient::Response: ctor\x1b[0m");
}

Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&& other) noexcept = default;

void Response::reset() noexcept
{
   if (impl)
   {
      logd("\x1b[34mClient::Response: dtor\x1b[0m");
      impl->destroy();
      impl.reset();
   }
}

Response::~Response() { reset(); }

// -------------------------------------------------------------------------------------------------

int Response::status_code() const noexcept { return impl->status_code(); }

void Response::async_read_some_any(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler)
{
   if (impl)
      impl->async_read_some(buffer, std::move(handler));
   else
      std::move(handler)(boost::asio::error::bad_descriptor, 0);
}

// =================================================================================================

Client::Client(Executor executor, Config config)
   : impl(std::make_unique<Client::Impl>(std::move(executor), std::move(config)))
{
}

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

Client::~Client() = default;

// -------------------------------------------------------------------------------------------------

void Client::async_connect_any(ConnectHandler&& handler)
{
   impl->async_connect(std::move(handler));
}

Executor Client::get_executor() const noexcept { return impl->get_executor(); }

#ifdef HAVE_CAPY
namespace detail
{
// Awaitable wrapper for async_connect that yields io_result<Session>
struct connect_awaitable
{
   std::shared_ptr<CapyAwaitableState<Session>> state;
   std::shared_ptr<Client::Impl> client_impl;

   bool await_ready() const noexcept { return false; }

   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
   {
      state->env = env;
      state->continuation = {h};

      // Capture state and env in the callback lambda
      client_impl->async_connect(
         [state_cap = state, env_cap = env](boost::system::error_code ec, Session session) mutable {
            state_cap->ec = ec;
            state_cap->result = std::move(session);
            env_cap->executor.dispatch(state_cap->continuation);
         });

      return std::noop_coroutine();
   }

   capy::io_result<Session> await_resume() noexcept { return state->resume_result(); }
};
} // namespace detail

auto Client::async_connect_capy(boost::corosio::endpoint ep)
{
   auto state = std::make_shared<CapyAwaitableState<Session>>();
   return detail::connect_awaitable{state, impl};
}
#endif

// =================================================================================================

} // namespace anyhttp::client
