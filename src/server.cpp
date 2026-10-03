
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

#include <boost/asio/buffer.hpp>

namespace anyhttp::server
{

// =================================================================================================

Request::Request(std::shared_ptr<Request::Impl> impl) : Reader(std::move(impl))
{
   if (*this)
      logd("[{}] \x1b[1;35mServer::Request: ctor\x1b[0m", pimpl().log_prefix());
}

Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&& other) noexcept = default;

void Request::reset() noexcept
{
   if (*this)
   {
      logd("[{}] \x1b[35mServer::Request: dtor\x1b[0m", pimpl().log_prefix());
      Reader::reset();
   }
}

Request::~Request() { reset(); }

// -------------------------------------------------------------------------------------------------

Request::Impl& Request::pimpl() const noexcept { return static_cast<Impl&>(Reader::pimpl()); }

std::string_view Request::method() const noexcept { return pimpl().method(); }
boost::url_view Request::url() const { return pimpl().url(); }
const Fields& Request::fields() const { return pimpl().fields(); }

// =================================================================================================

Response::Response(std::shared_ptr<Response::Impl> impl) : Writer(std::move(impl))
{
   if (*this)
      logd("[{}] \x1b[1;35mServer::Response: ctor\x1b[0m", pimpl().log_prefix());
}

Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&& other) noexcept = default;

void Response::reset() noexcept
{
   if (*this)
   {
      logd("[{}] \x1b[35mServer::Response: dtor\x1b[0m", pimpl().log_prefix());
      Writer::reset();
   }
}

Response::~Response() { reset(); }

// -------------------------------------------------------------------------------------------------

Response::Impl& Response::pimpl() const noexcept { return static_cast<Impl&>(Writer::pimpl()); }

Task<std::tuple<error_code>> Response::submit(unsigned int status_code, Fields headers)
{
   return pimpl().submit(status_code, std::move(headers));
}

#if !ANYHTTP_COROSIO
void Response::async_submit_any(StatusHandler&& handler, unsigned int status_code,
                                const Fields& headers)
{
   pimpl().async_submit(std::move(handler), status_code, headers);
}
#endif

// =================================================================================================

Server::Server(Executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
   impl->start();
}

Server::Server(Server&& other) noexcept = default;
Server& Server::operator=(Server&& other) noexcept
{
   if (this != &other)
   {
      if (impl)
         impl->destroy();
      impl = std::move(other.impl);
   }
   return *this;
}

Server::~Server()
{
   if (impl) // not moved from
      impl->destroy();
}

// -------------------------------------------------------------------------------------------------

void Server::on_request(RequestHandler&& handler) { impl->on_request(std::move(handler)); }

Executor Server::get_executor() const noexcept { return impl->get_executor(); }

asio::ip::tcp::endpoint Server::local_endpoint() const { return impl->local_endpoint(); }

// =================================================================================================

} // namespace anyhttp::server
