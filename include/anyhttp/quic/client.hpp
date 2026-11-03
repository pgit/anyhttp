#pragma once

//
// The client's end of QUIC, as seen from the protocols running on top: one coroutine that
// establishes a connection and hands back the session running it (src/quic/client.cpp).
//

#include "anyhttp/common.hpp"
#include "anyhttp/quic/backend.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace anyhttp::quic
{

// =================================================================================================

/// What a QUIC connection is made with, beyond where it goes.
struct ClientConfig
{
   /// The application protocol to negotiate, e.g. "h3".
   std::string alpn;

   /// The host the server's certificate has to be valid for: a name, sent as SNI, or an address.
   std::string host;

   /// The CA certificates to verify the server with, see client::Config::tls_ca_file.
   std::string tls_ca_file;

   /// How long the connection may be silent before it is considered dead.
   std::chrono::nanoseconds idle_timeout = std::chrono::seconds{30};
};

//
// Connects over QUIC to the first of `endpoints` that gets there, one after the other, with the
// application `factory` makes on top, and returns the session once that application is up
// (QuicApplication::on_ready()). Throws if it doesn't get there: with what ended the wait if the
// caller cancelled it, which also closes the connection, and with errc::connection_refused if the
// connections failed.
//
Task<std::shared_ptr<QuicSession>> async_connect(Executor executor,
                                                 std::vector<TcpEndpoint> endpoints,
                                                 ClientConfig config, ApplicationFactory factory);

/// The same with what `host`:`port` resolves to.
Task<std::shared_ptr<QuicSession>> async_connect(Executor executor, std::string host,
                                                 std::string port, ClientConfig config,
                                                 ApplicationFactory factory);

// =================================================================================================

} // namespace anyhttp::quic
