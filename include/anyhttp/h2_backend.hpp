#pragma once

//
// The HTTP/2 backend as seen from the generic server and client: factories that turn a stream
// that is ready to carry HTTP/2 into a Session::Impl. Everything else about the backend --
// nghttp2 and the session templates driving it -- stays behind h2_session.hpp
// and h2_session.cpp, so that dispatching to HTTP/2 needs no nghttp2 type here.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"
#include "anyhttp/stream_traits.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/url/url.hpp>

#include <memory>
#include <optional>
#include <string>

namespace anyhttp::nghttp2
{

// =================================================================================================

/**
 * A request received as HTTP/1.1 with "Upgrade: h2c" (RFC 7540, section 3.2) that has been answered
 * with "101 Switching Protocols". The HTTP/2 session continues it as stream 1. Only requests without
 * a body are upgraded, so the stream starts out half-closed (remote).
 */
struct Upgrade
{
   std::string settings; ///< decoded payload of the HTTP2-Settings header
   std::string method;
   boost::urls::url url;
   Fields fields; ///< request headers, without the connection-specific ones
};

//
// The stream is moved into the session, which runs on the stream's own executor -- hence the
// rvalue reference, which also keeps the SocketStream constraint from matching an lvalue.
//
// These are defined in src/h2_session.cpp and explicitly instantiated there for each of the
// stream types below, so that nghttp2 is instantiated in that one place only.
//

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server, Stream&& stream,
                                                   std::optional<Upgrade> upgrade = {});

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client, Stream&& stream);

extern template std::shared_ptr<Session::Impl>
make_server_session<boost::asio::ip::tcp::socket>(server::Server::Impl&,
                                                  boost::asio::ip::tcp::socket&&,
                                                  std::optional<Upgrade>);
extern template std::shared_ptr<Session::Impl>
make_server_session<SslStream>(server::Server::Impl&, SslStream&&, std::optional<Upgrade>);
extern template std::shared_ptr<Session::Impl>
make_server_session<any_async_stream>(server::Server::Impl&, any_async_stream&&,
                                    std::optional<Upgrade>);

extern template std::shared_ptr<Session::Impl>
make_client_session<boost::asio::ip::tcp::socket>(client::Client::Impl&,
                                                  boost::asio::ip::tcp::socket&&);

// =================================================================================================

} // namespace anyhttp::nghttp2
