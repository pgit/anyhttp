#pragma once

//
// The HTTP/2 backend as seen from the generic server and client: factories that turn a stream
// that is ready to carry HTTP/2 into a Session::Impl. Everything else about the backend --
// nghttp2 and the session templates driving it -- stays behind h2_session.hpp
// and h2_session.cpp, so that dispatching to HTTP/2 needs no nghttp2 type here.
//

#include "anyhttp/any_async_stream.hpp"
#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <memory>

namespace anyhttp::nghttp2
{

// =================================================================================================

using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   boost::asio::ip::tcp::socket&& socket);

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   SslStream&& stream);

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   AnyAsyncStream&& stream);

// -------------------------------------------------------------------------------------------------

std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client,
                                                   boost::asio::any_io_executor executor,
                                                   boost::asio::ip::tcp::socket&& socket);

// =================================================================================================

} // namespace anyhttp::nghttp2
