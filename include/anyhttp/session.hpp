#pragma once

#include "client.hpp"
#include "common.hpp"

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
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();

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
    * TODO: There is only a single Session interface for both server and client. This even might
    *       make sense for HTTP/2, where the server can also (sort of) submit a push promise to the
    *       client. But in general, it may be better to separate them.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Submit) CompletionToken = DefaultCompletionToken>
   auto async_submit(boost::urls::url url, const Fields& headers = {},
                     CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, Submit>(
         [](SubmitHandler handler, Session* self, boost::urls::url url, const Fields& headers) { //
            self->async_submit_any(std::move(handler), std::move(url), std::move(headers));
         },
         token, this, url, headers);
   }

private:
   void async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers);

   /// FIXME: For the user-facing interface, we don't want shared semantics by default.
   std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
