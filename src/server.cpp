
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

namespace anyhttp::server
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl))
{
   logd("\x1b[1;35mServer::Request: ctor\x1b[0m");
}
Request::Request(Request&&) noexcept = default;
Request::~Request()
{
   if (!m_impl)
      return;

   logd("\x1b[35mServer::Request: dtor\x1b[0m");

   auto impl = m_impl.get();
   impl->destroy(std::move(m_impl)); // give implementation a chance for cancellation
}

boost::url_view Request::url() const
{
   assert(m_impl);
   return m_impl->url();
}

std::optional<size_t> Request::content_length() const noexcept
{
   assert(m_impl);
   return m_impl->content_length();
}

void Request::async_read_some_any(ReadSomeHandler&& handler)
{
   assert(m_impl);
   m_impl->async_read_some(std::move(handler));
}

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : m_impl(std::move(impl))
{
   logd("\x1b[1;35mServer::Response: ctor\x1b[0m");
}
Response::Response(Response&&) noexcept = default;
Response::~Response()
{
   if (!m_impl)
      return;

   logd("\x1b[35mServer::Response: dtor\x1b[0m");

   auto impl = m_impl.get();
   impl->destroy(std::move(m_impl)); // give implementation a chance for cancellation
   m_impl.reset(); // if destroy() didn't
}

void Response::content_length(std::optional<size_t> content_length)
{
   assert(m_impl);
   m_impl->content_length(content_length);
}

void Response::async_submit_any(WriteHandler&& handler, unsigned int status_code, Fields headers)
{
   assert(m_impl);
   m_impl->async_submit(std::move(handler), status_code, std::move(headers));
}

void Response::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   assert(m_impl);
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

const asio::any_io_executor& Server::executor() const { return impl->executor(); }

asio::ip::tcp::endpoint Server::local_endpoint() const { return impl->local_endpoint(); }

Server::~Server() { impl->destroy(); }

// =================================================================================================

} // namespace anyhttp::server
