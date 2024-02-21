
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

namespace anyhttp::server
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl)) {}
Request::Request(Request&&) noexcept = default;
Request::~Request() = default;

boost::url_view Request::url() const { return m_impl->url(); }

std::optional<size_t> Request::content_length() const noexcept { return m_impl->content_length(); }

void Request::async_read_some_any(ReadSomeHandler&& handler)
{
   m_impl->async_read_some(std::move(handler));
}

const asio::any_io_executor& Request::executor() const { return m_impl->executor(); }

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : m_impl(std::move(impl)) {}
Response::Response(Response&&) noexcept = default;
Response::~Response() = default;

void Response::content_length(std::optional<size_t> content_length)
{
   m_impl->content_length(content_length);
}

void Response::write_head(unsigned int status_code, Fields headers)
{
   m_impl->write_head(status_code, std::move(headers));
}

void Response::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   m_impl->async_write(std::move(handler), buffer);
}

// =================================================================================================

Server::Server(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
   impl->run();
}

void Server::setRequestHandler(RequestHandler&& handler)
{
   impl->setRequestHandler(std::move(handler));
}

void Server::setRequestHandlerCoro(RequestHandlerCoro&& handler)
{
   impl->setRequestHandler(std::move(handler));
}

asio::ip::tcp::endpoint Server::local_endpoint() const { return impl->local_endpoint(); }

Server::~Server() { impl->destroy(); }

// =================================================================================================

} // namespace anyhttp::server
