//
// Stand-ins for the HTTP/3 backend in a CAPY build, until it is ported (docs/capy-port-plan.md,
// step 5d): a server that offers no HTTP/3, and a client that can not connect over it. An ASIO
// build has the real thing, and this file is empty there.
//

#include "anyhttp/runtime.hpp"

#if ANYHTTP_CAPY

#include "anyhttp/h3_backend.hpp"

namespace anyhttp::server
{

std::shared_ptr<Http3Server> make_http3_server(Server::Impl&, const asio::ip::udp::endpoint&)
{
   return nullptr;
}

} // namespace anyhttp::server

namespace anyhttp::client
{

Task<std::shared_ptr<Session::Impl>> async_connect_http3(Executor, std::string, std::string,
                                                         const Config&)
{
   throw_error(make_error_code(errc::operation_not_supported));
   co_return nullptr;
}

} // namespace anyhttp::client

#endif // ANYHTTP_CAPY
