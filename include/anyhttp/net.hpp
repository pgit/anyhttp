#pragma once

//
// The network half of the runtime layer, see anyhttp/runtime.hpp: TCP sockets, the acceptor, name
// resolution, TLS over TCP, what the sessions need from the stream they run on, and UDP sockets.
//
//    TcpSocket, TcpAcceptor, TlsContext, TlsStream, PlainServerStream
//    ANYHTTP_SERVER_STREAMS(X), ANYHTTP_CLIENT_STREAMS(X)   -- what the backends instantiate for
//    SocketStream, stream_traits<>, is_tls(), async_teardown()
//    io::shutdown(), io::cancel(), io::close(), io::remote_endpoint()
//    io::make_socket(), io::make_acceptor(), io::listen(), io::local_endpoint(), io::accept()
//    io::no_delay(), io::buffer_sizes(), io::resolve(), io::connect()
//    make_server_tls_context(), io::make_tls_stream(), io::handshake(), io::alpn(), io::tls_info()
//    io::drain()                                             -- below
//    UdpSocket, io::make_udp_socket(), io::open(), io::cancel(), io::close()
//    io::bind(), io::connect(), io::local_endpoint(), io::set_option(), io::send()   -- below
//    Signal                                                  -- a wake-up for any thread
//
// detail/net_asio.hpp has the documentation of each, except of those defined below. Addresses and
// endpoints are Boost.Asio's in both runtimes: they are plain values.
//

#include "anyhttp/common.hpp"

#if ANYHTTP_COROSIO
#include "anyhttp/detail/net_corosio.hpp"
#else
#include "anyhttp/detail/net_asio.hpp"
#endif

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>

#include <sys/socket.h>

namespace anyhttp::io
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
 */
template <SocketStream Stream>
Task<size_t> drain(Stream& stream, const Executor& executor,
                   std::chrono::steady_clock::duration timeout)
{
   Timer timer(executor);
   timer.arm(timeout, [&stream] { io::cancel(stream); });

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
// What the HTTP/3 backend does with its UdpSocket the same way in both runtimes, on the native
// handle: neither runtime has every socket option it needs, and it sends and receives with
// sendmsg() and recvmsg() anyway.
//

/// Binds \p socket to \p endpoint.
error_code bind(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept;

/// Connects \p socket to \p endpoint: it sends there and receives from there only.
error_code connect(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept;

/// The address \p socket is bound to. Throws what fails.
asio::ip::udp::endpoint local_endpoint(UdpSocket& socket);

/// Sets the integer socket option \p name at \p level to \p value.
error_code set_option(UdpSocket& socket, int level, int name, int value) noexcept;

/// Sends \p buffer on the connected \p socket, without waiting: when its send buffer is full,
/// that is errc::operation_would_block.
error_code send(UdpSocket& socket, asio::const_buffer buffer) noexcept;

} // namespace anyhttp::io
