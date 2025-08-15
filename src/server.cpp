
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

#include <boost/asio/buffer.hpp>

namespace anyhttp::server
{

// =================================================================================================

Request::Request(std::unique_ptr<Request::Impl> impl) : impl(std::move(impl))
{
   logd("\x1b[1;35mServer::Request: ctor\x1b[0m");
}
Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&& other) noexcept = default;
void Request::reset() noexcept
{
   if (impl)
   {
      logd("\x1b[35mServer::Request: dtor\x1b[0m");
      auto temp = impl.get();
      temp->destroy(std::move(impl)); // give implementation a chance for cancellation
   }
}
Request::~Request() { reset(); }

boost::url_view Request::url() const
{
   assert(impl);
   return impl->url();
}

std::optional<size_t> Request::content_length() const noexcept
{
   assert(impl);
   return impl->content_length();
}

void Request::async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler)
{
   assert(impl);
   impl->async_read_some(buffer, std::move(handler));
}

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : impl(std::move(impl))
{
   logd("\x1b[1;35mServer::Response: ctor\x1b[0m");
}
Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&& other) noexcept = default;
void Response::reset() noexcept
{
   if (impl)
   {
      logd("\x1b[35mServer::Response: dtor\x1b[0m");
      auto temp = impl.get();
      temp->destroy(std::move(impl)); // give implementation a chance for cancellation
      assert(!impl);
   }
}
Response::~Response() { reset(); }

void Response::content_length(std::optional<size_t> content_length)
{
   assert(impl);
   impl->content_length(content_length);
}

void Response::async_submit_any(WriteHandler&& handler, unsigned int status_code,
                                const Fields& headers)
{
   assert(impl);
   impl->async_submit(std::move(handler), status_code, std::move(headers));
}

void Response::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   assert(impl);
   impl->async_write(std::move(handler), buffer);
}

// =================================================================================================

Server::Server(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
   impl->start();
}

Server::~Server() { impl->destroy(); }

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

// =================================================================================================

} // namespace anyhttp::server
