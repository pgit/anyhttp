#pragma once

#include "client.hpp"
#include "common.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/url.hpp>

namespace anyhttp
{

// =================================================================================================

using Submit = void(boost::system::error_code, client::Request);
using SubmitHandler = boost::asio::any_completion_handler<Submit>;

using Get = void(boost::system::error_code, client::Message);
using GetHandler = boost::asio::any_completion_handler<Get>;

class Session
{
public:
   class Impl;
   Session() = default;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   void reset() noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl); }

public:
   /**
    * Submits a new request.
    *
    * Submitting a request means initiating the sending of the request method, path and any headers.
    * It does not mean that all or even any of those are actually transmitted immediately. The same
    * is true if you start an asynchronous write operation on the request object. The request may
    * just be queued for later transmission.
    *
    * Use \ref Request::async_get_response() on the request to wait for the response.
    *
    * A session may limit the number of requests in progress. When that limit is reached, this
    * does not wait for a request to finish -- the caller might be the one who has to finish it --
    * but fails immediately with \c asio::error::would_block. HTTP/1.1 is such a protocol, with a
    * limit of one: a request is in progress until it is \e complete, that is, until all of it,
    * header and body, has been written. See README.md, "Concurrent Requests".
    *
    * After a request could not be completed, an HTTP/1.1 session can't send any more requests,
    * and this fails with \c asio::error::connection_aborted.
    *
    * TODO: There is only a single Session interface for both server and client. This even might
    *       make sense for HTTP/2, where the server can also (sort of) submit a push promise to the
    *       client. But in general, it may be better to separate them.
    *
    * TODO: Look at
    * https://www.boost.org/doc/libs/latest/doc/html/boost_asio/example/cpp20/operations/composed_5.cpp
    *       and fully implement all requirements for asynchronous operations.
    *
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Submit) CompletionToken = DefaultCompletionToken>
   auto async_submit(boost::urls::url url, const Fields& headers = {},
                     CompletionToken&& token = CompletionToken())
   {
      auto executor = asio::get_associated_executor(token, get_executor());
      return asio::async_initiate<CompletionToken, Submit>(
         asio::bind_executor(
            executor,
            [this](auto&& handler, boost::urls::url url, const Fields& headers) { //
               async_submit_any(std::move(handler), std::move(url), headers);
            }),
         token, std::move(url), headers);
   }

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
    * the body while it arrives, a request body of its own, or a method other than GET still wants
    * \ref async_submit().
    *
    * The request goes out with "Content-Length: 0" unless \p headers already frames a body, so
    * that HTTP/1.1 does not have to make it chunked.
    *
    * Any error along the way completes this operation: the ones \ref async_submit() describes,
    * \c http::error::header_limit for a response header section over
    * \c client::Config::max_header_size, and \c http::error::partial_message for a body cut
    * short. The message that comes with an error is empty, and says \c status::unknown rather
    * than the 200 a default-constructed Beast response would claim. A response that says 404, on
    * the other hand, is not an error -- it is a response, and arrives as one.
    */
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

   boost::asio::any_io_executor get_executor() const noexcept;

private:
   void async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers);
   void async_get_any(GetHandler&& handler, boost::urls::url url, const Fields& headers);
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp
