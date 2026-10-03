
#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <utility>

namespace anyhttp::client
{

// =================================================================================================

/// An empty handle, as released by reset().
Request::Request() : Writer(nullptr) {}

Request::Request(std::unique_ptr<Request::Impl> impl) : Writer(std::move(impl))
{
   if (*this)
      logd("[{}] \x1b[1;34mClient::Request: ctor\x1b[0m", pimpl().log_prefix());
}

Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&& other) noexcept = default;

void Request::reset() noexcept
{
   if (*this)
   {
      logd("[{}] \x1b[34mClient::Request: dtor\x1b[0m", pimpl().log_prefix());
      Writer::reset();
   }
}

Request::~Request() { reset(); }

// -------------------------------------------------------------------------------------------------

Request::Impl& Request::pimpl() const noexcept { return static_cast<Impl&>(Writer::pimpl()); }

static Task<std::tuple<error_code, Response>> no_request()
{
   co_return std::tuple{errors::bad_descriptor, Response{}};
}

Task<std::tuple<error_code, Response>> Request::get_response()
{
   return *this ? pimpl().get_response() : no_request();
}

#if !ANYHTTP_COROSIO
void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   if (*this)
      pimpl().async_get_response(std::move(handler));
   else
      std::move(handler)(errors::bad_descriptor, Response{nullptr});
}
#endif

// =================================================================================================

/// An empty handle, as released by reset().
Response::Response() : Reader(nullptr) {}

Response::Response(std::unique_ptr<Response::Impl> impl) : Reader(std::move(impl))
{
   if (*this)
      logd("[{}] \x1b[1;34mClient::Response: ctor\x1b[0m", pimpl().log_prefix());
}

Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&& other) noexcept = default;

void Response::reset() noexcept
{
   if (*this)
   {
      logd("[{}] \x1b[34mClient::Response: dtor\x1b[0m", pimpl().log_prefix());
      Reader::reset();
   }
}

Response::~Response() { reset(); }

// -------------------------------------------------------------------------------------------------

Response::Impl& Response::pimpl() const noexcept { return static_cast<Impl&>(Reader::pimpl()); }

int Response::status_code() const noexcept { return pimpl().status_code(); }
const Fields& Response::fields() const { return pimpl().fields(); }

// =================================================================================================

Client::Client(Executor executor, Config config)
   : impl(std::make_unique<Client::Impl>(std::move(executor), std::move(config)))
{
}

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

Client::~Client() = default;

// -------------------------------------------------------------------------------------------------

Task<std::tuple<error_code, Session>> Client::connect()
{
   try
   {
      co_return std::tuple{error_code{}, co_await impl->async_connect()};
   }
   catch (const system_error& ex)
   {
      co_return std::tuple{ex.code(), Session{}};
   }
}

#if !ANYHTTP_COROSIO
void Client::async_connect_any(ConnectHandler&& handler)
{
   impl->async_connect(std::move(handler));
}
#endif

Executor Client::get_executor() const noexcept { return impl->get_executor(); }

// =================================================================================================

} // namespace anyhttp::client
