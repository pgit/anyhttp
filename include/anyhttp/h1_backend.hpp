#pragma once

//
// The HTTP/1.1 backend as seen from the generic server and client: factories that turn a stream
// that is ready to carry HTTP/1.1 into a Session::Impl. Everything else about the backend --
// beast's parser, serializer and the session templates driving them -- stays in h1_session.hpp
// and h1_session.cpp, which are the only places instantiating them.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"
#include "anyhttp/stream_traits.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <memory>

namespace anyhttp::beast_impl
{

// =================================================================================================

//
// The stream is moved into the session, which runs on the stream's own executor -- hence the
// rvalue reference, which also keeps the SocketStream constraint from matching an lvalue.
//
// These are defined in src/h1_session.cpp and explicitly instantiated there for each of the
// stream types below, so that beast's HTTP machinery is instantiated in that one place only.
//

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server, Stream&& stream);

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client, Stream&& stream);

extern template std::shared_ptr<Session::Impl>
make_server_session<boost::asio::ip::tcp::socket>(server::Server::Impl&,
                                                  boost::asio::ip::tcp::socket&&);
extern template std::shared_ptr<Session::Impl> make_server_session<SslStream>(server::Server::Impl&,
                                                                              SslStream&&);
extern template std::shared_ptr<Session::Impl>
make_server_session<any_async_stream>(server::Server::Impl&, any_async_stream&&);

extern template std::shared_ptr<Session::Impl>
make_client_session<boost::asio::ip::tcp::socket>(client::Client::Impl&,
                                                  boost::asio::ip::tcp::socket&&);

// =================================================================================================

} // namespace anyhttp::beast_impl
