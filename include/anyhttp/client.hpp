#pragma once

#include "common.hpp" // IWYU pragma: keep
#include "reader.hpp"
#include "writer.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include <boost/url.hpp>

namespace anyhttp
{
class Session;
namespace client
{

// =================================================================================================

struct Config
{
   // FIXME: the client does not connect to an URL, it connects to a host:port or endpoint
   boost::urls::url url{"localhost:8080"};
   Protocol protocol{Protocol::h2};

   //
   // The largest header section of a response the client accepts, in bytes, counted as for
   // server::Config::max_header_size. For a response with more, async_get_response() fails with
   // boost::beast::http::error::header_limit and the stream is reset -- with HTTP/1.1, which has no
   // streams, the connection can not be used any more.
   //
   size_t max_header_size = default_max_header_size;

   //
   // Whether to take up an HTTP/3 alternative service the server advertises (RFC 7838), as an
   // "Alt-Svc" header field on a response or, over HTTP/2, an ALTSVC frame. QUIC has no in-band
   // upgrade, so this is what an "upgrade" to HTTP/3 comes down to: the connection that learns
   // about the alternative keeps speaking what it speaks, and the *next* async_connect() goes to
   // the advertised endpoint over HTTP/3 instead of using \c protocol.
   //
   // The alternative is remembered for as long as its "ma" parameter says, which is 24 hours
   // unless the server gives one, and only for as long as the Client itself lives -- there is no
   // cache on disk, so a fresh process starts over with \c protocol.
   //
   bool follow_alt_svc = false;
};

// =================================================================================================

//
// A response received in one piece: status, header fields and the whole body as a string.
//
// This is a plain Beast message, with nothing of anyhttp left in it -- what \c Session::async_get()
// hands back. Its version is 11 whatever the protocol was: HTTP/2 and HTTP/3 have no version on
// the wire, and a Beast message has nowhere else to put one.
//
using Message = boost::beast::http::response<boost::beast::http::string_body>;

// -------------------------------------------------------------------------------------------------

class Response : public Reader
{
public:
   class Impl;
   Response();
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   void reset() noexcept;
   ~Response();

public:
   int status_code() const noexcept;

   /// The response header fields, without HTTP/2 and HTTP/3 pseudo-headers.
   const Fields& fields() const;

private:
   /// Hides Reader::pimpl(), narrowing it to the implementation this handle was built from.
   Impl& pimpl() const noexcept;
};

static_assert(boost::beast::is_async_read_stream<Response>::value);

// -------------------------------------------------------------------------------------------------

class Request : public Writer
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   void reset() noexcept;
   ~Request();

public:
   using GetResponse = void(boost::system::error_code, Response);
   using GetResponseHandler = asio::any_completion_handler<GetResponse>;

   /**
    * Waits for the response to this request, until its header has been received.
    *
    * With HTTP/1.1, responses arrive in the order the requests were sent, one after the other.
    * Getting the response to a request whose predecessors' responses have not been read to their
    * end does not wait for that to happen, but fails immediately with
    * \c asio::error::would_block. After a response could not be read -- it was released before
    * its end, or its request was released without asking for it -- getting any later response
    * fails with \c asio::error::connection_aborted. See README.md, "Concurrent Requests".
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(GetResponse) CompletionToken = DefaultCompletionToken>
   auto async_get_response(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, GetResponse>(
         asio::bind_executor(executor,
                             [this](auto&& handler) { //
                                async_get_response_any(std::move(handler));
                             }),
         token);
   }

private:
   void async_get_response_any(GetResponseHandler&& handler);

   /// Hides Writer::pimpl(), narrowing it to the implementation this handle was built from.
   Impl& pimpl() const noexcept;
};

// static_assert(boost::beast::is_async_write_stream<Request>::value);

// =================================================================================================

using Connect = void(boost::system::error_code, Session);
using ConnectHandler = asio::any_completion_handler<Connect>;

class Client
{
public:
   class Impl;
   Client(asio::any_io_executor executor, Config config);
   Client(Client&& other) noexcept;
   Client& operator=(Client&& other) noexcept;
   ~Client();

   using executor_type = asio::any_io_executor;
   executor_type get_executor() const noexcept;

   /**
    * Connect to configured peer and establish a new session.
    *
    * This operation supports 'terminal' cancellation. When cancellation is requested, it may
    * take some time to be executed. This is because the async resolver eventually calls
    * \c getaddrinfo(), which is a blocking system call. This is done from a separate thread,
    * so the users executor is not blocked, but this still means that the operation cannot be
    * interrupted.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken = DefaultCompletionToken>
   auto async_connect(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Connect>( //
         bind_executor(executor, [&](auto&& handler) { async_connect_any(std::move(handler)); }),
         token);
   }

private:
   void async_connect_any(ConnectHandler&& handler);
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace client
} // namespace anyhttp
