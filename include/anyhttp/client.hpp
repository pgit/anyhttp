#pragma once

#include "common.hpp" // IWYU pragma: keep
#include "reader.hpp"
#include "writer.hpp"

#include <boost/asio/buffer.hpp>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include <boost/url.hpp>

#include <string>
#include <vector>

namespace anyhttp
{
class Session;
namespace client
{

// =================================================================================================

/// What a \ref Client is made with.
struct Config
{
   /// What this client is for: the server's certificate has to be valid for the host of this URL.
   /// Unless \c endpoints says otherwise, it also is where the client connects to.
   boost::urls::url url{"localhost:8080"};

   /**
    * Where to connect, instead of resolving the host of \c url, for a caller that knows better:
    * it uses a resolver of its own, or follows a DNS HTTPS record (RFC 9460). They are tried in
    * this order until one of them can be connected to, and with these, \c url does not have to
    * resolve at all.
    *
    * Only where the connection goes changes. The certificate still has to be valid for the host of
    * \c url, which also goes out as SNI. And what a request says it is for is the URL it is
    * submitted with, as ever. An HTTP/3 alternative service is not followed, see
    * \c follow_alt_svc: the caller has made that choice.
    *
    * With HTTP/3, an endpoint that does not answer at all takes the idle timeout of 30 seconds
    * before the next one is tried -- limit that by cancelling \c async_connect().
    */
   std::vector<TcpEndpoint> endpoints{};

   /**
    * What to speak. An "https" \c url means TLS, and the server has a say in it: \c h1 offers
    * HTTP/1.1 only, while \c h2 offers HTTP/2 and HTTP/1.1 and takes what the server picks, so
    * the session may turn out to speak HTTP/1.1, see \c Session::protocol(). A server that does
    * not use ALPN at all gets HTTP/1.1, too.
    *
    * Any other scheme means cleartext, and exactly the protocol given: HTTP/2 is then "h2c" by
    * prior knowledge, which a server has to be ready for. HTTP/3 is TLS over QUIC with any scheme.
    */
   Protocol protocol{Protocol::h2};

   /**
    * The largest header section of a response the client accepts, in bytes, counted as for
    * \c server::Config::max_header_size. For a response with more, \c async_get_response() fails
    * with \c errors::header_limit and the stream is reset -- with HTTP/1.1,
    * which has no streams, the connection can not be used any more.
    */
   size_t max_header_size = default_max_header_size;

   /**
    * The kernel's send and receive buffer, in bytes, for the TCP connection; zero (the default)
    * leaves the kernel's autotuning alone. Set once the connection is up -- the socket is not open
    * before that -- so the window scale of the handshake is the kernel's own, and only the window
    * advertised from then on follows this. TCP only -- HTTP/3 goes over UDP.
    *
    * As with \c server::Config::socket_send_buffer_size, this is a testing aid: it bounds how much
    * \c async_write() can hand to the kernel before it has to wait for the peer, which over
    * HTTP/1.1 is the only thing that makes a write block at all. Both ends have to be pinned to
    * bound the pipeline -- the bytes in flight are the client's send buffer plus the server's
    * receive buffer, and whatever the server echoes back fills the other direction too.
    */
   int socket_send_buffer_size = 0;

   /// See \ref socket_send_buffer_size.
   int socket_receive_buffer_size = 0;

   /**
    * Whether to take up an HTTP/3 alternative service the server advertises (RFC 7838), as an
    * "Alt-Svc" header field on a response or, over HTTP/2, an ALTSVC frame. QUIC has no in-band
    * upgrade, so this is what an "upgrade" to HTTP/3 comes down to: the connection that learns
    * about the alternative keeps speaking what it speaks, and the *next* async_connect() goes to
    * the advertised endpoint over HTTP/3 instead of using \c protocol.
    *
    * The alternative is remembered for as long as its "ma" parameter says, which is 24 hours
    * unless the server gives one, and only for as long as the Client itself lives -- there is no
    * cache on disk, so a fresh process starts over with \c protocol. It does not apply to a client
    * that has been given \c endpoints.
    */
   bool follow_alt_svc = false;

   /**
    * The CA certificates to verify the server against over TLS, as a PEM file. Empty uses the
    * default trust store of the TLS library. The server's certificate must be valid for the host of
    * \c url -- also when connecting to an alternative service, as RFC 7838 requires -- or
    * \c async_connect() fails. Read on every connect, so a replaced file takes effect with the
    * next connection.
    */
   std::string tls_ca_file;
};

// =================================================================================================

/**
 * A response received in one piece: status, header fields and the whole body as a string.
 *
 * This is a plain Beast message, with nothing of anyhttp left in it -- what \c Session::async_get()
 * hands back. Its version is 11 whatever the protocol was: HTTP/2 and HTTP/3 have no version on
 * the wire, and a Beast message has nowhere else to put one.
 */
using Message = boost::beast::http::response<boost::beast::http::string_body>;

// -------------------------------------------------------------------------------------------------

/**
 * The response to a request, as a handle: its status and header fields, and a \ref Reader for its
 * body. It can be moved but not copied.
 *
 * Releasing one before the end of the body resets its stream with HTTP/2 and HTTP/3, which leaves
 * the session usable. Over HTTP/1.1, it is the end of the connection for further responses.
 */
class Response : public Reader
{
public:
   /// \cond internal
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   Response& operator=(Response&& other) noexcept;
   ~Response();
   /// \endcond

   /// A handle without a response, as the one that comes with an error.
   Response();

   /// Releases the response, as the destructor does.
   void reset() noexcept;

public:
   /// \return The status code of the response.
   int status_code() const noexcept;

   /// \return The response header fields, without HTTP/2 and HTTP/3 pseudo-headers.
   const Fields& fields() const;

private:
   /// Hides Reader::pimpl(), narrowing it to the implementation this handle was built from.
   Impl& pimpl() const noexcept;
};

// -------------------------------------------------------------------------------------------------

/**
 * A request that has been submitted, as a handle: a \ref Writer for its body, and the way to its
 * response. It can be moved but not copied.
 *
 * The body is ended with \ref Writer::async_write_eof, which does no harm where there is none.
 * What releasing a request before that means, or before asking for its response, is on the page on
 * concurrent requests.
 */
class Request : public Writer
{
public:
   /// \cond internal
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   Request& operator=(Request&& other) noexcept;
   ~Request();
   /// \endcond

   /// A handle without a request, as the one that comes with an error.
   Request();

   /// Releases the request, as the destructor does.
   void reset() noexcept;

public:
   /// The completion signature of \c async_get_response: the error and the response.
   using GetResponse = void(error_code, Response);
   using GetResponseHandler = Completion<GetResponse>;

   /**
    * Waits for the response to this request, until its header has been received.
    *
    * With HTTP/1.1, responses arrive in the order the requests were sent, one after the other.
    * Getting the response to a request whose predecessors' responses have not been read to their
    * end does not wait for that to happen, but fails immediately with
    * \c errors::would_block. After a response could not be read -- it was released before
    * its end, or its request was released without asking for it -- getting any later response
    * fails with \c errors::connection_aborted. See the page on concurrent requests.
    *
    * \param token The completion token, for a handler of the signature \c GetResponse.
    * \return What \p token makes of the operation.
    */
#if ANYHTTP_ASIO
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
#endif

   /// \c async_get_response() as a coroutine:
   /// <tt>auto [ec, response] = co_await request.get_response();</tt>
   /// \return The error and the response.
   IoTask<Response> get_response();

private:
#if ANYHTTP_ASIO
   void async_get_response_any(GetResponseHandler&& handler);
#endif

   /// Hides Writer::pimpl(), narrowing it to the implementation this handle was built from.
   Impl& pimpl() const noexcept;
};

// static_assert(boost::beast::is_async_write_stream<Request>::value);

// =================================================================================================

/// The completion signature of \c Client::async_connect: the error and the session.
using Connect = void(error_code, Session);
using ConnectHandler = Completion<Connect>;

/**
 * Makes connections to the one server of its \ref Config, each of them a \ref Session. It can be
 * moved but not copied.
 */
class Client
{
public:
   /// \cond internal
   class Impl;
   Client(Client&& other) noexcept;
   Client& operator=(Client&& other) noexcept;
   ~Client();
   /// \endcond

   /// A client on \p executor, which does nothing until it is told to connect.
   /// \param executor What the client and its sessions run on.
   /// \param config What it connects to, and how.
   Client(Executor executor, Config config);

   /// The executor type, for what looks for one.
   using executor_type = Executor;

   /// \return The executor the client runs on.
   executor_type get_executor() const noexcept;

   /**
    * Connect to configured peer and establish a new session.
    *
    * This operation supports 'terminal' cancellation. When cancellation is requested, it may
    * take some time to be executed. This is because the async resolver eventually calls
    * \c getaddrinfo(), which is a blocking system call. This is done from a separate thread,
    * so the users executor is not blocked, but this still means that the operation cannot be
    * interrupted.
    *
    * \param token The completion token, for a handler of the signature \c Connect.
    * \return What \p token makes of the operation.
    */
#if ANYHTTP_ASIO
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken = DefaultCompletionToken>
   auto async_connect(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Connect>( //
         bind_executor(executor, [&](auto&& handler) { async_connect_any(std::move(handler)); }),
         token);
   }
#endif

   /// \c async_connect() as a coroutine: <tt>auto [ec, session] = co_await client.connect();</tt>
   /// \return The error and the session.
   IoTask<Session> connect();

private:
#if ANYHTTP_ASIO
   void async_connect_any(ConnectHandler&& handler);
#endif
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace client
} // namespace anyhttp
