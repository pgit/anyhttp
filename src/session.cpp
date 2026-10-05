#include "anyhttp/session.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/buffer.hpp>

#include <array>

using namespace boost::asio;

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : impl(std::move(impl))
{
   // logd("Session::ctor: use_count={}", impl_.use_count());
}

// -------------------------------------------------------------------------------------------------

Session::Session(Session&& other) noexcept : impl(std::move(other.impl))
{
   // logd("Session::move: use_count={}", impl_.use_count());
}

Session& Session::operator=(Session&& other) noexcept
{
   if (this != &other)
   {
      impl = std::move(other.impl);
   }
   logd("Session::move: use_count={}", impl.use_count());
   return *this;
}

void Session::reset() noexcept
{
   if (impl)
   {
      impl->destroy();
      impl.reset();
   }
}

Session::~Session() { reset(); }

// -------------------------------------------------------------------------------------------------

Executor Session::get_executor() const noexcept { return impl->get_executor(); }

#if ANYHTTP_ASIO
void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
{
   impl->async_submit(std::move(handler), "POST", std::move(url), headers);
}
#endif

// =================================================================================================

namespace
{

//
// The whole of async_get(), as the coroutine it reads best as: submit, end the empty request
// body, wait for the response, read all of it. Its result is what Session::get() yields, so every
// error is reported, not thrown.
//
// The implementation is held by shared_ptr: a Session released while the GET is still in flight
// must not pull the ground out from under the operations still running on it.
//
Task<std::tuple<error_code, client::Message>> get_message(std::shared_ptr<Session::Impl> session,
                                                          boost::urls::url url, Fields headers)
{
   //
   // What an error leaves behind: an empty message that says status::unknown, rather than the 200
   // a default-constructed Beast response would claim.
   //
   auto failed = [](error_code ec) {
      client::Message message;
      message.result(boost::beast::http::status::unknown);
      return std::tuple{ec, std::move(message)};
   };

   //
   // A GET has no body, and saying so with a "Content-Length: 0" keeps HTTP/1.1 from framing one
   // as chunked -- which would leave the request incomplete until the write_eof() below, and the
   // session unable to take another one until then.
   //
   if (!headers.count(boost::beast::http::field::content_length) &&
       !headers.count(boost::beast::http::field::transfer_encoding))
      headers.set(boost::beast::http::field::content_length, "0");

   //
   // The public Session::submit() is POST-only, but every backend can send whatever method it is
   // handed, see Session::Impl::async_submit().
   //
   auto [ec, request] = co_await session->submit("GET", std::move(url), std::move(headers));
   if (ec)
      co_return failed(ec);

   if (auto [ec] = co_await request.write_eof(); ec)
      co_return failed(ec);

   auto [response_ec, response] = co_await request.get_response();
   if (response_ec)
      co_return failed(response_ec);

   client::Message message;
   message.result(static_cast<unsigned>(response.status_code()));
   for (auto&& field : response.fields())
      message.insert(field.name_string(), field.value()); // insert(), so repeated fields survive

   auto& body = message.body();
   std::array<char, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.read_some(asio::buffer(buffer));
      body.append(buffer.data(), n);

      if (ec == errors::eof)
         break;
      else if (ec)
         co_return failed(ec);
   }

   logd("async_get: {} {}, {} bytes", message.result_int(), message.reason(), body.size());
   co_return std::tuple{error_code{}, std::move(message)};
}

} // namespace

Task<std::tuple<error_code, client::Request>> Session::submit(boost::urls::url target,
                                                              Fields headers)
{
   return impl->submit("POST", std::move(target), std::move(headers));
}

Task<std::tuple<error_code, client::Message>> Session::get(boost::urls::url url, Fields headers)
{
#if ANYHTTP_COROSIO
   //
   // A task runs on its caller's executor. This one runs on the session's -- its strand, if it
   // has one -- as async_get_any() does with ASIO, and the caller resumes on its own afterwards.
   //
   co_return co_await capy::run(impl->get_executor())(
      get_message(impl, std::move(url), std::move(headers)));
#else
   return get_message(impl, std::move(url), std::move(headers));
#endif
}

#if ANYHTTP_ASIO
void Session::async_get_any(GetHandler&& handler, boost::urls::url url, const Fields& headers)
{
   launch(get_executor(), get_message(impl, std::move(url), headers), std::move(handler));
}
#endif

// =================================================================================================

} // namespace anyhttp
