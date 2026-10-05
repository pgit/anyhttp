//
// The network half of the runtime layer for COROSIO, see anyhttp/net.hpp: what is not inline in
// detail/net_corosio.hpp. net_asio.cpp is its counterpart, in the same order, and net.cpp has what
// both have in common.
//
#include "anyhttp/runtime.hpp"

#if ANYHTTP_COROSIO

#include "anyhttp/net.hpp"

#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/logging.hpp"

#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>

#include <sys/socket.h>

namespace anyhttp
{

// =================================================================================================

TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key)
{
   TlsContext ctx;
   if (auto ec = ctx.set_min_protocol_version(corosio::tls_version::tls_1_3))
      throw_error(ec);
   if (auto ec = ctx.use_certificate_chain_file(certificate_chain))
      throw std::system_error(ec, "use_certificate_chain_file");
   if (auto ec = ctx.use_private_key_file(private_key, corosio::tls_file_format::pem))
      throw std::system_error(ec, "use_private_key_file");
   std::ignore = ctx.set_alpn({"h2", "http/1.1"});
   return ctx;
}

// -------------------------------------------------------------------------------------------------

namespace io
{

void listen(TcpAcceptor& acceptor, const asio::ip::tcp::endpoint& endpoint, bool reuse_port)
{
   const auto family = endpoint.address().is_v4() ? corosio::family::v4 : corosio::family::v6;
   if (auto ec = acceptor.open(family))
      throw std::system_error(ec, "open");
   acceptor.set_option(corosio::socket_option::reuse_address(true));
   if (reuse_port)
      acceptor.set_option(corosio::socket_option::reuse_port(true));

   // Accept IPv4 clients on an IPv6 listener, too, see net_asio.cpp.
   if (family == corosio::family::v6)
   {
      try
      {
         acceptor.set_option(corosio::socket_option::v6_only(false));
      }
      catch (const std::system_error& ex)
      {
         logw("[{}] error enabling dual-stack on {}: {}", log_prefix(Role::server), endpoint,
              ex.what());
      }
   }

   if (auto ec = acceptor.bind(to_corosio(endpoint)))
      throw std::system_error(ec, "bind");
   if (auto ec = acceptor.listen())
      throw std::system_error(ec, "listen");
}

std::pair<int, int> buffer_sizes(TcpSocket& socket)
{
   int send = 0, receive = 0;
   socklen_t len = sizeof(int);
   ::getsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDBUF, &send, &len);
   len = sizeof(int);
   ::getsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &receive, &len);
   return {send, receive};
}

Task<std::tuple<error_code, std::vector<asio::ip::tcp::endpoint>>>
resolve(Executor executor, std::string host, std::string port)
{
   corosio::resolver resolver(executor.context());
   auto [ec, results] =
      co_await resolver.resolve(host, port, corosio::resolve_flags::numeric_service);

   std::vector<asio::ip::tcp::endpoint> endpoints;
   for (auto&& entry : results)
      endpoints.push_back(to_asio(entry));
   co_return std::tuple{ec, std::move(endpoints)};
}

Task<std::tuple<error_code, asio::ip::tcp::endpoint>>
connect(TcpSocket& socket, std::vector<asio::ip::tcp::endpoint> endpoints)
{
   error_code ec = make_error_code(errc::host_unreachable);
   for (const auto& endpoint : endpoints)
   {
      socket.close();
      if ((ec =
              socket.open(endpoint.address().is_v4() ? corosio::family::v4 : corosio::family::v6)))
         continue;
      std::tie(ec) = co_await socket.connect(to_corosio(endpoint));
      if (!ec)
         co_return std::tuple{error_code{}, endpoint};
   }
   co_return std::tuple{ec, asio::ip::tcp::endpoint{}};
}

std::string tls_info(TlsStream& stream)
{
   return std::format("{}, ALPN={}", stream.tls().name(), stream.tls().alpn_protocol());
}

} // namespace io

// =================================================================================================

} // namespace anyhttp

#endif // ANYHTTP_COROSIO
