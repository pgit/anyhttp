#pragma once

//
// The HTTP/1.1 backend as seen from the generic server and client: factories that turn a stream
// that is ready to carry HTTP/1.1 into a Session::Impl. Everything else about the backend --
// beast's parser, serializer and the session templates driving them -- stays in h1/session.hpp
// and h1/session.cpp, which are the only places instantiating them.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/net.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <memory>

namespace anyhttp::beast_impl
{

// =================================================================================================

//
// The stream is moved into the session -- hence the rvalue reference, which also keeps the
// SocketStream constraint from matching an lvalue. The session runs on \p executor, the
// connection's strand if it has one: a corosio socket knows its context, not its strand.
//
// These are defined in src/h1/session.cpp and explicitly instantiated there for each of the
// stream types below, so that beast's HTTP machinery is instantiated in that one place only.
//

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server, Executor executor,
                                                   Stream&& stream);

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client, Executor executor,
                                                   Stream&& stream);

#define ANYHTTP_H1_SERVER(Stream)                                                                  \
   extern template std::shared_ptr<Session::Impl> make_server_session<Stream>(                     \
      server::Server::Impl&, Executor, Stream&&);
#define ANYHTTP_H1_CLIENT(Stream)                                                                  \
   extern template std::shared_ptr<Session::Impl> make_client_session<Stream>(                     \
      client::Client::Impl&, Executor, Stream&&);
ANYHTTP_SERVER_STREAMS(ANYHTTP_H1_SERVER)
ANYHTTP_CLIENT_STREAMS(ANYHTTP_H1_CLIENT)
#undef ANYHTTP_H1_SERVER
#undef ANYHTTP_H1_CLIENT

// =================================================================================================

} // namespace anyhttp::beast_impl
