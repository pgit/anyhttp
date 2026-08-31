#pragma once

//
// The HTTP/3 backend as seen from the generic server and client. ngtcp2 and nghttp3 stay behind
// this header, inside the h3_* files: on the server side as an opaque Http3Server owning the
// shared UDP socket, its receive loop and the QUIC connection-ID demux (src/h3_server.cpp), on
// the client side as a single coroutine that establishes a QUIC connection and hands back the
// session running on it (src/h3_client.cpp).
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/udp.hpp>

#include <memory>
#include <string>

namespace anyhttp::server
{

// =================================================================================================

//
// The server's HTTP/3 half: one UDP socket shared by all QUIC connections, the receive loop
// demultiplexing datagrams onto them by connection ID, and the connections themselves. Sessions
// register with the owning Server::Impl just like the TCP-based ones, so they take part in
// server-wide shutdown.
//
class Http3Server
{
public:
   virtual ~Http3Server() = default;

   /// Starts the UDP receive loop.
   virtual void start() = 0;

   /// Closes the socket and tears down all QUIC connections, each sending a CONNECTION_CLOSE.
   virtual void destroy() = 0;
};

/// Binds the UDP socket for HTTP/3 to `endpoint`, usually the address and port the TCP acceptor
/// is already listening on, so that all three protocols share one endpoint.
std::shared_ptr<Http3Server> make_http3_server(Server::Impl& server,
                                               const boost::asio::ip::udp::endpoint& endpoint);

// =================================================================================================

} // namespace anyhttp::server

namespace anyhttp::client
{

// =================================================================================================

/// Connects to `host`:`port` over QUIC and returns the HTTP/3 session running on it.
boost::asio::awaitable<std::shared_ptr<Session::Impl>>
async_connect_http3(boost::asio::any_io_executor executor, std::string host, std::string port);

// =================================================================================================

} // namespace anyhttp::client
