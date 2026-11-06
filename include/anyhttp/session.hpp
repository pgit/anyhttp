#pragma once

#include "client.hpp"
#include "common.hpp"

#include <boost/url.hpp>

#include <string>
#include <string_view>

namespace anyhttp
{

// =================================================================================================

/// The completion signature of \c Session::async_submit: the error and the request.
using Submit = void(error_code, client::Request);
using SubmitHandler = Completion<Submit>;

/// The completion signature of \c Session::async_get: the error and the whole response.
using Get = void(error_code, client::Message);
using GetHandler = Completion<Get>;

/// The completion signature of \c Session::async_shutdown.
using SessionShutdown = void(error_code);
using SessionShutdownHandler = Completion<SessionShutdown>;

/**
 * One connection of a client, as a handle: what \ref client::Client::async_connect gives. Requests
 * are submitted on it, as many at a time as the protocol and the peer allow.
 *
 * It can be moved but not copied, and destroying it ends the connection the abrupt way, see
 * \ref async_shutdown. It is to be used on its executor only. The requests and responses it has
 * given out may outlive it, and fail from then on.
 */
class Session
{
public:
   /// \cond internal
   class Impl;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();
   /// \endcond

   /// A handle without a connection.
   Session() = default;

   /// Ends the connection the abrupt way, as the destructor does: nothing asynchronous, and the
   /// peer sees a connection that was cut.
   void reset() noexcept;

   /// \return Whether this handle has a connection.
   explicit constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

public:
   /**
    * Submits a new request.
    *
    * Submitting a request means initiating the sending of the request method, path and any headers.
    * It does not mean that all or even any of those are actually transmitted immediately. The same
    * is true if you start an asynchronous write operation on the request object. The request may
    * just be queued for later transmission.
    *
    * Use \ref client::Request::async_get_response on the request to wait for the response.
    *
    * A session may limit the number of requests in progress. When that limit is reached, this
    * does not wait for a request to finish -- the caller might be the one who has to finish it --
    * but fails immediately with \c errors::would_block. HTTP/1.1 is such a protocol, with a
    * limit of one: a request is in progress until it is \e complete, that is, until all of it,
    * header and body, has been written. See the page on concurrent requests.
    *
    * After a request could not be completed, an HTTP/1.1 session can't send any more requests,
    * and this fails with \c errors::connection_aborted.
    *
    * Without a method, this is a POST. There is an overload for any other, see below.
    *
    * \param target The URL of the request.
    * \param headers The header fields of the request.
    * \param token The completion token, for a handler of the signature \c Submit.
    * \return What \p token makes of the operation.
    */
   //
   // TODO: There is only a single Session interface for both server and client. This even might
   //       make sense for HTTP/2, where the server can also (sort of) submit a push promise to the
   //       client. But in general, it may be better to separate them.
   //
   // TODO: Look at
   // https://www.boost.org/doc/libs/latest/doc/html/boost_asio/example/cpp20/operations/composed_5.cpp
   //       and fully implement all requirements for asynchronous operations.
   //
#if ANYHTTP_ASIO
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Submit) CompletionToken = DefaultCompletionToken>
   auto async_submit(boost::urls::url target, const Fields& headers = {},
                     CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Submit>(
         asio::bind_executor(
            executor,
            [this](auto&& handler, boost::urls::url url, const Fields& headers) { //
               async_submit_any(std::move(handler), "POST", std::move(url), headers);
            }),
         token, std::move(target), headers);
   }

   /**
    * Submits a new request with the given \p method. All else is as with the overload above, and
    * ends the same way: \ref client::Request::async_write_eof() ends the request body, even if
    * there is none, and \ref client::Request::async_get_response() waits for the response.
    *
    * GET and HEAD go out with "Content-Length: 0" unless \p headers already frames a body, so that
    * HTTP/1.1 does not have to make them chunked, which some servers refuse. With any other method,
    * a request without framing has a chunked body over HTTP/1.1.
    *
    * This is the way to have the body of a response without keeping all of it, as
    * \ref async_get() does, when a peer may send more than the caller wants to read. What it gets
    * to send is bounded by flow control, and over HTTP/1.1 by the socket buffers. Releasing the
    * session ends the rest of it, see the programmer's guide.
    *
    * \param method The method of the request.
    * \param target The URL of the request.
    * \param headers The header fields of the request.
    * \param token The completion token, for a handler of the signature \c Submit.
    * \return What \p token makes of the operation.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Submit) CompletionToken = DefaultCompletionToken>
   auto async_submit(std::string_view method, boost::urls::url target, const Fields& headers = {},
                     CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Submit>(
         asio::bind_executor(executor,
                             [this](auto&& handler, const std::string& method, boost::urls::url url,
                                    const Fields& headers) { //
                                async_submit_any(std::move(handler), method, std::move(url),
                                                 headers);
                             }),
         token, std::string(method), std::move(target), headers);
   }
#endif

   /**
    * Performs a whole GET request in one operation, and hands back the whole response.
    *
    * This is the short way through what \ref async_submit() spreads over four steps: it submits
    * the request, ends its (empty) body, waits for the response and reads all of it into a
    * \c client::Message -- status, header fields and body as a \c std::string:
    *
    * \code
    *    auto message = co_await session.async_get(url);
    *    EXPECT_EQ(message.result_int(), 200);
    *    EXPECT_EQ(message.body(), "Hello, World!");
    * \endcode
    *
    * The convenience is paid for with memory: the body is buffered in full, however large it
    * turns out to be, as there is no way to look at it before it is complete. Anything that needs
    * the body while it arrives, or to stop reading it at some point, a request body of its own, or
    * a method other than GET still wants \ref async_submit().
    *
    * The request goes out with "Content-Length: 0" unless \p headers already frames a body, so
    * that HTTP/1.1 does not have to make it chunked.
    *
    * Any error along the way completes this operation: the ones \ref async_submit() describes,
    * \c errors::header_limit for a response header section over
    * \c client::Config::max_header_size, and \c errors::partial_message for a body cut
    * short. The message that comes with an error is empty, and says \c status::unknown rather
    * than the 200 a default-constructed Beast response would claim. A response that says 404, on
    * the other hand, is not an error -- it is a response, and arrives as one.
    *
    * \param url The URL of the request.
    * \param headers The header fields of the request.
    * \param token The completion token, for a handler of the signature \c Get.
    * \return What \p token makes of the operation.
    */
#if ANYHTTP_ASIO
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Get) CompletionToken = DefaultCompletionToken>
   auto async_get(boost::urls::url url, const Fields& headers = {},
                  CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Get>(
         asio::bind_executor(
            executor,
            [this](auto&& handler, boost::urls::url url, const Fields& headers) { //
               async_get_any(std::move(handler), std::move(url), headers);
            }),
         token, std::move(url), headers);
   }
#endif

   /**
    * Ends the connection in an orderly way, and completes once it is closed. Unlike destroying the
    * session, which is over as fast as possible, this tells the peer that nothing has been lost:
    *
    * - HTTP/2 sends a GOAWAY, and TLS over TCP sends its "close_notify" and waits for the peer's.
    *   Then comes the FIN, and the socket is closed once the peer has sent its own. Each of those
    *   waits is limited to two seconds, so a peer that does not cooperate can only delay this.
    *   How the peer has ended its side is not reported: there is nothing to be done about it, and
    *   not every TLS library can tell.
    * - HTTP/3 sends a CONNECTION_CLOSE, which has no answer to wait for.
    *
    * It does not wait for anything in progress: a request or response that is not complete yet
    * fails, right away, and so does anything else tried with this session afterwards.
    *
    * The one error is \c errors::connection_aborted, for an end that could not be orderly: a
    * write has been interrupted, leaving part of a TLS record behind, for which a "close_notify"
    * would come too late. The connection is closed in that case as well.
    *
    * Calling this again does nothing, and neither does destroying the session afterwards.
    *
    * \param token The completion token, for a handler of the signature \c SessionShutdown.
    * \return What \p token makes of the operation.
    */
#if ANYHTTP_ASIO
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(SessionShutdown)
                CompletionToken = DefaultCompletionToken>
   auto async_shutdown(CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, SessionShutdown>(
         asio::bind_executor(executor,
                             [this](auto&& handler) { //
                                async_shutdown_any(std::move(handler));
                             }),
         token);
   }
#endif

   //
   // The coroutine spelling of the operations above, which both runtimes have: no completion
   // token, the result as a tuple, and errors reported, never thrown.
   //

   /// \c async_submit() as a coroutine:
   /// <tt>auto [ec, request] = co_await session.submit(url, headers);</tt>
   /// \param target The URL of the request.
   /// \param headers The header fields of the request.
   /// \return The error and the request.
   IoTask<client::Request> submit(boost::urls::url target, Fields headers = {});

   /// \c async_submit() with a method, as a coroutine:
   /// <tt>auto [ec, request] = co_await session.submit("GET", url, headers);</tt>
   /// \param method The method of the request.
   /// \param target The URL of the request.
   /// \param headers The header fields of the request.
   /// \return The error and the request.
   IoTask<client::Request> submit(std::string_view method, boost::urls::url target,
                                  Fields headers = {});

   /// \c async_get() as a coroutine: <tt>auto [ec, message] = co_await session.get(url);</tt>
   /// \param url The URL of the request.
   /// \param headers The header fields of the request.
   /// \return The error and the whole response.
   IoTask<client::Message> get(boost::urls::url url, Fields headers = {});

   /// \c async_shutdown() as a coroutine: <tt>auto [ec] = co_await session.shutdown();</tt>
   /// \return The error.
   IoTask<> shutdown();

   /**
    * The protocol this session speaks, which over TLS the server has a say in: see
    * \c client::Config::protocol. It tells how many requests the session can take at a time, as
    * HTTP/1.1 takes only one, see \ref async_submit().
    *
    * \return What it speaks.
    */
   Protocol protocol() const noexcept;

   /// \return The executor the session runs on, and is to be used on.
   Executor get_executor() const noexcept;

private:
#if ANYHTTP_ASIO
   void async_submit_any(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                         const Fields& headers);
   void async_get_any(GetHandler&& handler, boost::urls::url url, const Fields& headers);
   void async_shutdown_any(SessionShutdownHandler&& handler);
#endif
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp
