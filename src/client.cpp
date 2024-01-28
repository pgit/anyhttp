
#include <utility>

#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

namespace anyhttp::client
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl)) {}
Request::Request(Request&&) noexcept = default;
Request::~Request() = default;

void Request::write_head(unsigned int status_code, Headers headers)
{
   m_impl->write_head(status_code, std::move(headers));
}

void Request::async_write_any(WriteHandler&& handler, std::vector<std::uint8_t> buffer)
{
   m_impl->async_write(std::move(handler), std::move(buffer));
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

Request Client::submit(boost::urls::url url, Headers headers)
{
   return impl->submit(std::move(url), std::move(headers));
}

asio::ip::tcp::endpoint Client::local_endpoint() const { return impl->local_endpoint(); }

Client::~Client() = default;

// =================================================================================================

} // namespace anyhttp::client
