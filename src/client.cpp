
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
   logd("\x1b[1;34mClient::Request: ctor\x1b[0m");
}
Request::Request(Request&&) noexcept = default;
Request::~Request()
{
   if (impl)
   {
      logd("\x1b[34mClient::Request: dtor\x1b[0m");
      auto temp = impl.get();
      temp->destroy(std::move(impl)); // give implementation a chance for cancellation
   }
}

void Request::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (impl)
      impl->async_write(std::move(handler), buffer);
   else
      std::move(handler)(boost::asio::error::operation_aborted);
}

void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   if (impl)
      impl->async_get_response(std::move(handler));
   else
      std::move(handler)(boost::asio::error::operation_aborted, Response{nullptr});
}

const asio::any_io_executor& Request::executor() const
{
   assert(impl);
   return impl->executor();
}

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl_) : impl(std::move(impl_))
{
   logd("\x1b[1;34mClient::Response: ctor\x1b[0m");
}
Response::Response(Response&&) noexcept = default;
Response::~Response()
{
   if (impl)
   {
      logd("\x1b[34mClient::Response: dtor\x1b[0m");
      auto temp = impl.get();
      temp->destroy(std::move(impl)); // give implementation a chance for cancellation
   }
}

void Response::async_read_some_any(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler)
{
   assert(impl);
   impl->async_read_some(buffer, std::move(handler));
}

// =================================================================================================

Client::Client(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Client::Impl>(std::move(executor), std::move(config)))
{
}

Client::~Client() = default;

void Client::async_connect_any(ConnectHandler&& handler)
{
   assert(impl);
   impl->async_connect(std::move(handler));
}

const asio::any_io_executor& Client::executor() const { return impl->executor(); }

// =================================================================================================

} // namespace anyhttp::client
