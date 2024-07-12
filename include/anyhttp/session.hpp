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
    */
   template <boost::asio::completion_token_for<Submit> CompletionToken>
   auto async_submit(boost::urls::url url, Fields headers, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Submit>(
         [&](asio::completion_handler_for<Submit> auto handler, boost::urls::url url,
             Fields headers) { //
            async_submit_any(std::move(handler), url, std::move(headers));
         },
         token, url, headers);
   }

private:
   void async_submit_any(SubmitHandler&& handler, boost::urls::url url, Fields headers);

   std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
