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
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   void reset() noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(m_impl); }

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
      //
      // A custom callable like this that provides a get_executor() function enables the use of
      // the cancel_{at,after} completion token adapters.
      //
      struct initiation
      {
         Session* self;
         using executor_type = boost::asio::any_io_executor;
         executor_type get_executor() const noexcept {return self->get_executor();}

         auto operator()(SubmitHandler handler, boost::urls::url url, const Fields& headers) const
         {
            self->async_submit_any(std::move(handler), std::move(url), std::move(headers));
         };
      };
      return boost::asio::async_initiate<CompletionToken, Submit>(initiation{this}, token,
                                                                  std::move(url), headers);
   }

   boost::asio::any_io_executor get_executor() const noexcept;

private:
   void async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers);

   /// FIXME: For the user-facing interface, we don't want shared semantics by default.
   std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
