
#include "anyhttp/server.hpp"
#include "anyhttp/server_impl.hpp"

namespace anyhttp::server
{

Request::~Request() = default;
Response::~Response() = default;

Server::Server(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Server::Impl>(std::move(executor), std::move(config)))
{
}

void Server::setRequestHandler(RequestHandler&& handler)
{
   impl->setRequestHandler(std::move(handler));
}

ip::tcp::endpoint Server::local_endpoint() const { return impl->local_endpoint(); }

Server::~Server() = default;

} // namespace anyhttp::server
