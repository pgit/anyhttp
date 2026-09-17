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
         asio::bind_executor(executor,
                             [this](auto&& handler, boost::urls::url url, const Fields& headers) {// 
            async_submit_any(std::move(handler), std::move(url), headers);
         }),
         token, std::move(url), headers);
   }

   boost::asio::any_io_executor get_executor() const noexcept;

private:
   void async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers);
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp
