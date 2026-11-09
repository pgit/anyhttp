#pragma once

//
// The network half of the runtime layer, see anyhttp/runtime.hpp: TCP sockets, the acceptor, name
// resolution, TLS over TCP, what the sessions need from the stream they run on, and UDP sockets.
// Addresses and endpoints are the runtime's own, see anyhttp/runtime.hpp.
//
// Each runtime defines, in asio/net.hpp or corosio/net.hpp (documented in asio/net.hpp):
//
//    Types        TcpSocket, TcpAcceptor, TlsContext, TlsStream, UdpSocket, PlainServerStream,
//                 ANYHTTP_SERVER_STREAMS(X), ANYHTTP_CLIENT_STREAMS(X)
//    Streams      SocketStream, stream_traits<>, get_socket(), is_tls(), io::shutdown(),
//                 io::cancel(), io::close(), io::remote_endpoint(), async_teardown()
//    TCP          io::make_socket(), io::make_acceptor(), io::listen(), io::local_endpoint(),
//                 io::accept(), io::no_delay(), io::buffer_sizes(), io::set_buffer_sizes(),
//                 io::resolve(), io::connect()
//    TLS          make_server_tls_context(), make_client_tls_context(), io::make_tls_stream(),
//                 io::set_peer_name(), io::handshake(), io::alpn(), io::tls_info(),
//                 io::is_truncated()
//    UDP          io::make_udp_socket(), io::open(), io::cancel(), io::close()
//    Threads      Signal, a wake-up that may be sent from any thread
//
// Declared and documented here, the same for both runtimes:
//
//    Streams      io::drain()
//    Addresses    io::make_address(), SocketAddress, io::to_sockaddr(), io::from_sockaddr()
//                 (defined by each runtime, in src/asio/net.cpp and src/corosio/net.cpp)
//    UDP          io::bind(), io::connect(), io::local_endpoint(), io::set_option(), io::send()
//                 (defined once, in src/net.cpp, on the native handle)
//

#include "anyhttp/common.hpp"

#if ANYHTTP_COROSIO
#include "anyhttp/corosio/net.hpp"
#else
#include "anyhttp/asio/net.hpp"
#endif

#include <boost/asio/buffer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

#include <sys/socket.h>

// The runtime's own namespace, which io:: is part of: a plain "namespace anyhttp::io" here would
// be a namespace of its own to clang, which does not look through the inline namespace for it.
#if ANYHTTP_COROSIO
namespace anyhttp::inline corosio::io
#else
namespace anyhttp::inline asio::io
#endif
{

/**
 * Reads and drops what the socket under \p stream still receives, until the peer has ended its
 * side of the connection, or \p timeout has passed. Yields the number of bytes dropped.
 *
 * This is the last stage of ending a connection (RFC 9112, section 9.6; RFC 9113 has no rule of
 * its own, but the same TCP beneath it): after the FIN, and before letting go of the socket.
 * Closing a socket that still has unread data in its receive queue answers the peer with an RST
 * instead, and an RST may cost the peer what it has not read yet -- which can be the very response
 * or GOAWAY that said the connection was ending. A peer that ended the connection itself has
 * nothing left to send, and this costs one read.
 *
 * The data is of no interest, not even decrypted: this reads the socket itself, by hand, as that
 * is what every stream type has in common.
 *
 * \p owner is what keeps \p stream alive: a timeout that expires as the peer ends its side may
 * still cancel the stream after this has returned (see Timer), and does nothing once \p owner is
 * gone.
 */
template <SocketStream Stream>
Task<size_t> drain(Stream& stream, std::weak_ptr<const void> owner, const Executor& executor,
                   std::chrono::steady_clock::duration timeout)
{
   Timer timer(executor);
   timer.arm(timeout, [&stream, weak = std::move(owner)] {
      if (auto self = weak.lock())
         io::cancel(stream);
   });

   auto& socket = get_socket(stream);
   std::array<char, 16 * 1024> buffer;
   size_t drained = 0;
   for (;;)
   {
      if (auto [ec] = co_await io::wait_readable(socket); ec)
         break;
      auto n = ::recv(socket.native_handle(), buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (n == 0)
         break; // the peer has ended its side
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
         break;
      drained += std::max<ssize_t>(n, 0);
   }
   timer.cancel();
   co_return drained;
}

//
// Addresses and endpoints, as far as the runtimes differ in them. Declared here, and defined by
// each runtime in src/asio/net.cpp and src/corosio/net.cpp.
//

/// Parses the IPv4 or IPv6 address \p text. What fails is in \p ec.
IpAddress make_address(std::string_view text, error_code& ec) noexcept;

/// Parses the IPv4 or IPv6 address \p text. Throws what fails.
inline IpAddress make_address(std::string_view text)
{
   error_code ec;
   auto address = make_address(text, ec);
   if (ec)
      throw_error(ec);
   return address;
}

/// An endpoint in the form the socket API takes it: for the native handle, and for ngtcp2.
struct SocketAddress
{
   sockaddr_storage storage{};
   socklen_t size = 0;

   sockaddr* data() noexcept { return reinterpret_cast<sockaddr*>(&storage); }
   const sockaddr* data() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
};

/// \p endpoint as the socket API takes it.
SocketAddress to_sockaddr(const UdpEndpoint& endpoint) noexcept;

/// The endpoint the socket API reported, or nothing if it is not an IPv4 or IPv6 one.
std::optional<UdpEndpoint> from_sockaddr(const sockaddr* address, socklen_t size) noexcept;

//
// What the HTTP/3 backend does with its UdpSocket the same way in both runtimes, on the native
// handle: neither runtime has every socket option it needs, and it sends and receives with
// sendmsg() and recvmsg() anyway.
//

/// Binds \p socket to \p endpoint.
error_code bind(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept;

/// Connects \p socket to \p endpoint: it sends there and receives from there only.
error_code connect(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept;

/// The address \p socket is bound to. Throws what fails.
UdpEndpoint local_endpoint(UdpSocket& socket);

/// Sets the integer socket option \p name at \p level to \p value.
error_code set_option(UdpSocket& socket, int level, int name, int value) noexcept;

/// Sends \p buffer on the connected \p socket, without waiting: when its send buffer is full,
/// that is errc::operation_would_block.
error_code send(UdpSocket& socket, boost::asio::const_buffer buffer) noexcept;

} // namespace anyhttp::io (in the runtime's namespace)
