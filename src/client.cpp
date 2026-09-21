
#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <utility>

namespace anyhttp::client
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : Writer(std::move(impl))
{
   if (*this)
      logd("\x1b[1;34mClient::Request: ctor\x1b[0m");
}

Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&& other) noexcept = default;

void Request::reset() noexcept
{
   if (*this)
   {
      logd("\x1b[34mClient::Request: dtor\x1b[0m");
      Writer::reset();
   }
}

Request::~Request() { reset(); }

// -------------------------------------------------------------------------------------------------

Request::Impl& Request::pimpl() const noexcept { return static_cast<Impl&>(Writer::pimpl()); }

void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   if (*this)
      pimpl().async_get_response(std::move(handler));
   else
      std::move(handler)(boost::asio::error::bad_descriptor, Response{nullptr});
}

// =================================================================================================

Response::Response() = default;

Response::Response(std::unique_ptr<Response::Impl> impl) : Reader(std::move(impl))
{
   if (*this)
      logd("\x1b[1;34mClient::Response: ctor\x1b[0m");
}

Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&& other) noexcept = default;

void Response::reset() noexcept
{
   if (*this)
   {
      logd("\x1b[34mClient::Response: dtor\x1b[0m");
      Reader::reset();
   }
}

Response::~Response() { reset(); }

// -------------------------------------------------------------------------------------------------

Response::Impl& Response::pimpl() const noexcept { return static_cast<Impl&>(Reader::pimpl()); }

int Response::status_code() const noexcept { return pimpl().status_code(); }
const Fields& Response::fields() const { return pimpl().fields(); }

// =================================================================================================

Client::Client(boost::asio::any_io_executor executor, Config config)
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

asio::any_io_executor Client::get_executor() const noexcept { return impl->get_executor(); }

// =================================================================================================

} // namespace anyhttp::client
