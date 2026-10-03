//
// The network half of the runtime layer, see anyhttp/net.hpp. What is not inline in
// detail/net_asio.hpp or detail/net_corosio.hpp is here, for either runtime.
//

#include "anyhttp/net.hpp"

#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/logging.hpp"
#include "anyhttp/tls.hpp"

#include <openssl/ssl.h>

#include <span>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

#if ANYHTTP_COROSIO
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>

#include <netinet/tcp.h>
#else
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/v6_only.hpp>
#endif

namespace anyhttp
{

// =================================================================================================

#if !ANYHTTP_COROSIO

//
// The protocols we speak over TLS on TCP, in descending order of preference. HTTP/3 is not in
// here: it is offered on the UDP endpoint instead, see anyhttp/h3_backend.hpp.
//
// https://nghttp2.org/documentation/tutorial-server.html
//
static unsigned char next_proto_list[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

static unsigned int next_proto_list_len = sizeof(next_proto_list);
static int next_proto_cb(SSL* s, const unsigned char** data, unsigned int* len, void* arg)
{
   *data = next_proto_list;
   *len = next_proto_list_len;
   return SSL_TLSEXT_ERR_OK;
}

//
// ALPN, picking the first protocol of ours the client offers -- our preference wins, not the
// client's order.
//
static int alpn_select_proto_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                const unsigned char* in, unsigned int inlen, void* arg)
{
   for (std::string_view wanted : {"h2", "http/1.1"})
   {
      // The wire format is a sequence of length-prefixed, non-empty protocol names.
      for (auto list = std::span{in, inlen}; !list.empty() && list.size() > list[0];
           list = list.subspan(1 + list[0]))
      {
         if (std::string_view{reinterpret_cast<const char*>(&list[1]), list[0]} != wanted)
            continue;

         *out = &list[1];
         *outlen = list[0];
         return SSL_TLSEXT_ERR_OK;
      }
   }

   return SSL_TLSEXT_ERR_NOACK;
}

TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key)
{
   asio::ssl::context ctx{asio::ssl::context::tlsv13};
   SSL_CTX_set_next_protos_advertised_cb(ctx.native_handle(), next_proto_cb, nullptr);
   SSL_CTX_set_alpn_select_cb(ctx.native_handle(), alpn_select_proto_cb, nullptr);

   ctx.use_certificate_chain_file(certificate_chain);
   ctx.use_private_key_file(private_key, asio::ssl::context::pem);

   return ctx;
}

// -------------------------------------------------------------------------------------------------

namespace io
{

void listen(TcpAcceptor& acceptor, const asio::ip::tcp::endpoint& endpoint, bool reuse_port)
{
   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   if (reuse_port)
      acceptor.set_option(asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

   //
   // Accept IPv4 clients on an IPv6 listener, too. This has to go after open() -- there is no
   // socket to set it on before that -- and before bind(), which is when it takes effect. Not
   // fatal if it fails: most systems are dual-stack by default (net.ipv6.bindv6only=0) anyway.
   //
   if (endpoint.protocol() == asio::ip::tcp::v6())
   {
      error_code ec;
      acceptor.set_option(asio::ip::v6_only(false), ec);
      if (ec)
         logw("[{}] error enabling dual-stack on {}: {}", log_prefix(Role::server), endpoint,
              ec.what());
   }

   acceptor.bind(endpoint);
   acceptor.listen();
}

std::pair<int, int> buffer_sizes(TcpSocket& socket)
{
   asio::socket_base::send_buffer_size send;
   asio::socket_base::receive_buffer_size receive;
   error_code ec;
   socket.get_option(send, ec);
   socket.get_option(receive, ec);
   return {send.value(), receive.value()};
}

Task<std::tuple<error_code, std::vector<asio::ip::tcp::endpoint>>>
resolve(Executor executor, std::string host, std::string port)
{
   asio::ip::tcp::resolver resolver(executor);
   auto [ec, results] = co_await resolver.async_resolve(
      host, port, asio::ip::tcp::resolver::numeric_service, asio::as_tuple);

   std::vector<asio::ip::tcp::endpoint> endpoints;
   for (auto&& entry : results)
      endpoints.push_back(entry.endpoint());
   co_return std::tuple{ec, std::move(endpoints)};
}

Task<std::tuple<error_code, asio::ip::tcp::endpoint>>
connect(TcpSocket& socket, std::vector<asio::ip::tcp::endpoint> endpoints)
{
   co_return co_await asio::async_connect(socket, endpoints, asio::as_tuple);
}

std::string_view alpn(TlsStream& stream)
{
   const unsigned char* data = nullptr;
   unsigned int len = 0;
   SSL_get0_alpn_selected(stream.native_handle(), &data, &len);
   return data ? std::string_view(reinterpret_cast<const char*>(data), len) : std::string_view{};
}

std::string tls_info(TlsStream& stream) { return tls_handshake_info(stream.native_handle()); }

} // namespace io

#else // ANYHTTP_COROSIO

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

   // Accept IPv4 clients on an IPv6 listener, too, see the ASIO version.
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

void no_delay(TcpSocket& socket)
{
   int one = 1;
   ::setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

#endif // ANYHTTP_COROSIO

// =================================================================================================
// UDP, the same in both runtimes, see anyhttp/net.hpp.
// =================================================================================================

namespace io
{

error_code bind(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept
{
   if (::bind(socket.native_handle(), endpoint.data(), static_cast<socklen_t>(endpoint.size())))
      return last_error();
   return {};
}

error_code connect(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept
{
   if (::connect(socket.native_handle(), endpoint.data(), static_cast<socklen_t>(endpoint.size())))
      return last_error();
   return {};
}

asio::ip::udp::endpoint local_endpoint(UdpSocket& socket)
{
   asio::ip::udp::endpoint endpoint;
   auto size = static_cast<socklen_t>(endpoint.capacity());
   if (::getsockname(socket.native_handle(), endpoint.data(), &size))
      throw_error(last_error());
   endpoint.resize(size);
   return endpoint;
}

error_code set_option(UdpSocket& socket, int level, int name, int value) noexcept
{
   if (::setsockopt(socket.native_handle(), level, name, &value, sizeof(value)))
      return last_error();
   return {};
}

error_code send(UdpSocket& socket, asio::const_buffer buffer) noexcept
{
   if (::send(socket.native_handle(), buffer.data(), buffer.size(), 0) < 0)
      return last_error();
   return {};
}

} // namespace io

// =================================================================================================

} // namespace anyhttp
