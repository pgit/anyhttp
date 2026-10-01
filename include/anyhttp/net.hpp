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
//    UdpSocket, io::make_udp_socket(), io::open(), io::cancel(), io::close()
//    io::bind(), io::connect(), io::local_endpoint(), io::set_option(), io::send()   -- below
//    Signal                                                  -- a wake-up for any thread
//
// detail/net_asio.hpp has the documentation of each, except of those defined below. Addresses and
// endpoints are Boost.Asio's in both runtimes: they are plain values.
//

#include "anyhttp/common.hpp"

#if ANYHTTP_CAPY
#include "anyhttp/detail/net_capy.hpp"
#else
#include "anyhttp/detail/net_asio.hpp"
#endif

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/udp.hpp>

namespace anyhttp::io
{

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
