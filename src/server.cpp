
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

namespace anyhttp::server
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl)) {}
Request::Request(Request&&) noexcept = default;
Request::~Request() = default;

void Request::async_read_some_any(
   asio::any_completion_handler<void(std::vector<std::uint8_t>)>&& handler)
{
   m_impl->async_read_some(std::move(handler));
}

const asio::any_io_executor& Request::executor() const { return m_impl->executor(); }

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : m_impl(std::move(impl)) {}
Response::Response(Response&&) noexcept = default;
Response::~Response() = default;

void Response::write_head(unsigned int status_code, Headers headers)
{
   m_impl->write_head(status_code, std::move(headers));
}

void Response::async_write_any(asio::any_completion_handler<void()>&& handler,
                               std::vector<std::uint8_t> buffer)
{
   m_impl->async_write(std::move(handler), std::move(buffer));
}

// =================================================================================================

Server::Server(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
}

void Server::setRequestHandler(RequestHandler&& handler)
{
   impl->setRequestHandler(std::move(handler));
}

void Server::setRequestHandlerCoro(RequestHandlerCoro&& handler)
{
   impl->setRequestHandler(std::move(handler));
}

ip::tcp::endpoint Server::local_endpoint() const { return impl->local_endpoint(); }

Server::~Server() = default;

// =================================================================================================

} // namespace anyhttp::server
