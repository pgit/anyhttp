//
// The network half of the runtime layer for COROSIO, see anyhttp/net.hpp: what is not inline in
// corosio/net.hpp. asio/net.cpp is its counterpart, in the same order, and net.cpp has what
// both have in common.
//
#include "anyhttp/net.hpp"

#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/logging.hpp"

#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>

#include <cstring>
#include <optional>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace anyhttp::inline corosio
{

// =================================================================================================

//
// corosio's tls_context builds its SSL_CTX on the first handshake, and neither it nor the stream
// hands it out, so there is nothing to install a keylog callback on. Say so rather than fail: it
// is a debugging aid, and HTTP/3 logs all the same, on an SSL_CTX of our own.
//
static void warn_no_keylog(const std::string& keylog_file, Role role)
{
   if (!keylog_file.empty())
      logw("[{}] TLS key log '{}': not supported by corosio's TLS stream, only HTTP/3 logs",
           log_prefix(role), keylog_file);
}

TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key, const std::string& keylog_file)
{
   warn_no_keylog(keylog_file, Role::server);
   TlsContext ctx;
   if (auto ec = ctx.set_min_protocol_version(boost::corosio::tls_version::tls_1_3))
      throw_error(ec);
   if (auto ec = ctx.use_certificate_chain_file(certificate_chain))
      throw std::system_error(ec, "use_certificate_chain_file");
   if (auto ec = ctx.use_private_key_file(private_key, boost::corosio::tls_file_format::pem))
      throw std::system_error(ec, "use_private_key_file");
   std::ignore = ctx.set_alpn({"h2", "http/1.1"});
   return ctx;
}

// -------------------------------------------------------------------------------------------------

TlsContext make_client_tls_context(const std::string& ca_file, Protocol protocol,
                                   const std::string& keylog_file, bool verify)
{
   warn_no_keylog(keylog_file, Role::client);
   TlsContext ctx;
   if (auto ec = ctx.set_min_protocol_version(boost::corosio::tls_version::tls_1_2))
      throw_error(ec);
   if (verify)
   {
      if (auto ec =
             ca_file.empty() ? ctx.set_default_verify_paths() : ctx.load_verify_file(ca_file))
         throw std::system_error(ec, "load_verify_file");
   }
   if (auto ec = ctx.set_verify_mode(verify ? boost::corosio::tls_verify_mode::peer
                                            : boost::corosio::tls_verify_mode::none))
      throw_error(ec);
   if (protocol == Protocol::h2)
      std::ignore = ctx.set_alpn({"h2", "http/1.1"});
   else
      std::ignore = ctx.set_alpn({"http/1.1"});
   return ctx;
}

// -------------------------------------------------------------------------------------------------

namespace io
{

void listen(TcpAcceptor& acceptor, const TcpEndpoint& endpoint, bool reuse_port)
{
   const auto family =
      endpoint.address().is_v4() ? boost::corosio::family::v4 : boost::corosio::family::v6;
   if (auto ec = acceptor.open(family))
      throw std::system_error(ec, "open");
   if (auto ec = acceptor.set_option(boost::corosio::socket_option::reuse_address(true)))
      throw std::system_error(ec, "reuse_address");
   if (reuse_port)
   {
      if (auto ec = acceptor.set_option(boost::corosio::socket_option::reuse_port(true)))
         throw std::system_error(ec, "reuse_port");
   }

   // Accept IPv4 clients on an IPv6 listener, too, see asio/net.cpp.
   if (family == boost::corosio::family::v6)
   {
      if (auto ec = acceptor.set_option(boost::corosio::socket_option::v6_only(false)))
         logw("[{}] error enabling dual-stack on {}: {}", log_prefix(Role::server), endpoint,
              ec.message());
   }

   if (auto ec = acceptor.bind(endpoint))
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

void set_buffer_sizes(TcpSocket& socket, int send, int receive)
{
   if (send > 0)
      ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDBUF, &send, sizeof(send));
   if (receive > 0)
      ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &receive, sizeof(receive));
}

IoTask<std::vector<TcpEndpoint>> resolve(Executor executor, std::string host, std::string port)
{
   boost::corosio::resolver resolver(executor.context());
   auto [ec, results] =
      co_await resolver.resolve(host, port, boost::corosio::resolve_flags::numeric_service);

   std::vector<TcpEndpoint> endpoints;
   for (auto&& entry : results)
      endpoints.push_back(entry);
   co_return std::tuple{ec, std::move(endpoints)};
}

IoTask<TcpEndpoint> connect(TcpSocket& socket, std::vector<TcpEndpoint> endpoints,
                            std::optional<TcpEndpoint> local)
{
   error_code ec = make_error_code(
      !local || endpoints.empty() ? errc::host_unreachable : errc::address_family_not_supported);
   for (const auto& endpoint : endpoints)
   {
      if (local && endpoint.address().is_v4() != local->address().is_v4())
      {
         logd("not connecting to {}: not of the family of {}", endpoint, *local);
         continue;
      }

      socket.close();
      if ((ec = socket.open(endpoint.address().is_v4() ? boost::corosio::family::v4
                                                       : boost::corosio::family::v6)))
         continue;
      if (local && local->port() != 0 &&
          (ec = socket.set_option(boost::corosio::socket_option::reuse_address(true))))
         continue;
      if (local && (ec = socket.bind(*local)))
         continue;
      std::tie(ec) = co_await socket.connect(endpoint);
      if (!ec)
         co_return std::tuple{error_code{}, endpoint};
      if (ec == errc::operation_canceled) // and every further endpoint would be, too
         break;
   }
   co_return std::tuple{ec, TcpEndpoint{}};
}

std::string tls_info(TlsStream& stream)
{
   return std::format("{}, ALPN={}", stream.tls().name(), stream.tls().alpn_protocol());
}

} // namespace io

// =================================================================================================
// Addresses and endpoints, see anyhttp/net.hpp. corosio keeps the sockaddr form to itself.
// =================================================================================================

IpAddress normalize(IpAddress address)
{
   if (address.is_v6())
   {
      const auto v6 = address.to_v6();
      if (v6.is_v4_mapped())
         return v6.to_v4();
   }
   return address;
}

namespace io
{

IpAddress make_address(std::string_view text, error_code& ec) noexcept
{
   auto [result, address] = boost::corosio::make_ip_address(text);
   ec = result;
   return address;
}

SocketAddress to_sockaddr(const UdpEndpoint& endpoint) noexcept
{
   SocketAddress address;
   const auto ip = endpoint.address();
   if (ip.is_v4())
   {
      sockaddr_in in{};
      in.sin_family = AF_INET;
      in.sin_port = htons(endpoint.port());
      const auto bytes = ip.to_v4().to_bytes();
      std::memcpy(&in.sin_addr, bytes.data(), bytes.size());
      std::memcpy(&address.storage, &in, sizeof(in));
      address.size = sizeof(in);
   }
   else
   {
      const auto v6 = ip.to_v6();
      sockaddr_in6 in6{};
      in6.sin6_family = AF_INET6;
      in6.sin6_port = htons(endpoint.port());
      const auto bytes = v6.to_bytes();
      std::memcpy(&in6.sin6_addr, bytes.data(), bytes.size());
      in6.sin6_scope_id = v6.scope_id();
      std::memcpy(&address.storage, &in6, sizeof(in6));
      address.size = sizeof(in6);
   }
   return address;
}

std::optional<UdpEndpoint> from_sockaddr(const sockaddr* address, socklen_t size) noexcept
{
   if (address->sa_family == AF_INET && size >= sizeof(sockaddr_in))
   {
      sockaddr_in in;
      std::memcpy(&in, address, sizeof(in));
      boost::corosio::ipv4_address::bytes_type bytes;
      std::memcpy(bytes.data(), &in.sin_addr, bytes.size());
      return UdpEndpoint(boost::corosio::ipv4_address(bytes), ntohs(in.sin_port));
   }
   if (address->sa_family == AF_INET6 && size >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 in6;
      std::memcpy(&in6, address, sizeof(in6));
      boost::corosio::ipv6_address::bytes_type bytes;
      std::memcpy(bytes.data(), &in6.sin6_addr, bytes.size());
      return UdpEndpoint(boost::corosio::ipv6_address(bytes, in6.sin6_scope_id),
                         ntohs(in6.sin6_port));
   }
   return std::nullopt;
}

} // namespace io

// =================================================================================================

} // namespace anyhttp::inline corosio
