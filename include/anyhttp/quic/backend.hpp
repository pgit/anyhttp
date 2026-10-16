#pragma once

//
// QUIC as seen from the generic server and from the protocols running on top. ngtcp2 stays behind
// this header, inside quic/: on the server side as an opaque QuicServer owning the shared UDP
// socket, its receive loop and the connection-ID demux (src/quic/server.cpp), which hands each
// connection to the application registered for the ALPN it negotiates.
//

#include "anyhttp/server_impl.hpp"

#include <functional>
#include <memory>
#include <string>

namespace anyhttp::quic
{

class QuicApplication;
class QuicSession;

/// Makes the protocol running on top of `session`, see QuicApplication.
using ApplicationFactory = std::function<std::unique_ptr<QuicApplication>(QuicSession& session)>;

} // namespace anyhttp::quic

namespace anyhttp::server
{

// =================================================================================================

//
// The server's QUIC half: one UDP socket shared by all QUIC connections, the receive loop
// de-multiplexing datagrams onto them by connection ID, and the connections themselves.
//
// Sessions register with the owning Server::Impl just like the TCP-based ones, so they take part
// in server-wide shutdown.
//
class QuicServer
{
public:
   virtual ~QuicServer() = default;

   //
   // Serves connections that negotiate `alpn` with what `factory` makes. The order of the calls is
   // the server's preference, should a client offer more than one. Only before start().
   //
   virtual void add_application(std::string alpn, quic::ApplicationFactory factory) = 0;

   /// Starts the UDP receive loop.
   virtual void start() = 0;

   /// Closes the socket and tears down all QUIC connections, each sending a CONNECTION_CLOSE.
   virtual void destroy() = 0;
};

/// Binds the UDP socket to `endpoint`, usually the address and port the TCP acceptor is already
/// listening on, so that all three HTTP versions share one endpoint.
std::shared_ptr<QuicServer> make_quic_server(Server::Impl& server, const UdpEndpoint& endpoint);

// =================================================================================================

} // namespace anyhttp::server
