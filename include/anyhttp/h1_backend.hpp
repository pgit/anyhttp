#pragma once

//
// The HTTP/1.1 backend as seen from the generic server and client: factories that turn a stream
// that is ready to carry HTTP/1.1 into a Session::Impl. Everything else about the backend --
// beast's parser, serializer and the session templates driving them -- stays in h1_session.hpp
// and h1_session.cpp, which are the only places instantiating them.
//

#include "anyhttp/any_async_stream.hpp"
#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <memory>

namespace anyhttp::beast_impl
{

// =================================================================================================

using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   SslStream&& stream);

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   AnyAsyncStream&& stream);

std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server,
                                                   boost::asio::any_io_executor executor,
                                                   boost::asio::ip::tcp::socket&& socket);

std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client,
                                                   boost::asio::any_io_executor executor,
                                                   boost::asio::ip::tcp::socket&& socket);

// =================================================================================================

} // namespace anyhttp::beast_impl
