
#include <utility>

#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

namespace anyhttp::client
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl)) {}
Request::Request(Request&&) noexcept = default;
Request::~Request() = default;

void Request::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   m_impl->async_write(std::move(handler), buffer);
}

void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   m_impl->async_get_response(std::move(handler));
}

const asio::any_io_executor& Request::executor() const { return m_impl->executor(); }

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : m_impl(std::move(impl)) {}
Response::Response(Response&&) noexcept = default;
Response::~Response() = default;

void Response::async_read_some_any(ReadSomeHandler&& handler)
{
   m_impl->async_read_some(std::move(handler));
}

// =================================================================================================

Client::Client(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Client::Impl>(std::move(executor), std::move(config)))
{
}

Client::~Client() = default;


void Client::async_connect_any(ConnectHandler&& handler)
{
   impl->async_connect(std::move(handler));
}

const asio::any_io_executor& Client::executor() const { return impl->executor(); }
asio::ip::tcp::endpoint Client::local_endpoint() const { return impl->local_endpoint(); }

// =================================================================================================

} // namespace anyhttp::client
