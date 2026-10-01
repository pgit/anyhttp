#pragma once

//
// The network half of the runtime layer, see anyhttp/runtime.hpp: TCP sockets, the acceptor, name
// resolution, TLS over TCP, and what the sessions need from the stream they run on.
//
//    TcpSocket, TcpAcceptor, TlsContext, TlsStream, PlainServerStream
//    ANYHTTP_SERVER_STREAMS(X), ANYHTTP_CLIENT_STREAMS(X)   -- what the backends instantiate for
//    SocketStream, stream_traits<>, is_tls(), async_teardown()
//    io::shutdown(), io::cancel(), io::close(), io::remote_endpoint()
//    io::make_socket(), io::make_acceptor(), io::listen(), io::local_endpoint(), io::accept()
//    io::no_delay(), io::buffer_sizes(), io::resolve(), io::connect()
//    make_server_tls_context(), io::make_tls_stream(), io::handshake(), io::alpn(), io::tls_info()
//    Signal                                                  -- a wake-up for any thread
//
// detail/net_asio.hpp has the documentation of each. Addresses and endpoints are Boost.Asio's in
// both runtimes: they are plain values.
//

#include "anyhttp/common.hpp"

#if ANYHTTP_CAPY
#include "anyhttp/detail/net_capy.hpp"
#else
#include "anyhttp/detail/net_asio.hpp"
#endif
