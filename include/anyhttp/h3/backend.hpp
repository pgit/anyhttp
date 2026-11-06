#pragma once

//
// The HTTP/3 backend as seen from the generic server and client. nghttp3 stays behind this
// header, inside h3/: on the server side as the application the QUIC server runs for connections
// that negotiate "h3" (see anyhttp/quic/backend.hpp), on the client side as a single coroutine
// that establishes a QUIC connection and hands back the session running on it
// (src/h3/client.cpp).
//

#include "anyhttp/quic/backend.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <memory>
#include <string>
#include <vector>

namespace anyhttp::h3
{

// =================================================================================================

/// HTTP/3 for QuicServer::add_application(): its requests go to the request handler of `server`.
quic::ApplicationFactory make_http3_application(server::Server::Impl& server);

/// Connects to the first of `endpoints` that it can over QUIC, and returns the HTTP/3 session
/// running on it.
Task<std::shared_ptr<Session::Impl>> async_connect_http3(Executor executor,
                                                         std::vector<TcpEndpoint> endpoints,
                                                         const client::Config& config);

// =================================================================================================

} // namespace anyhttp::h3
