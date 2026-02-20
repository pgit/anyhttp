
#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

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

asio::any_io_executor Request::get_executor() const noexcept
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
