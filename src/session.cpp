#include "anyhttp/session.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>

#include <array>

using namespace boost::asio;

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : impl(std::move(impl))
{
   // logd("Session::ctor: use_count={}", m_impl.use_count());
}

// -------------------------------------------------------------------------------------------------

Session::Session(Session&& other) noexcept : impl(std::move(other.impl))
{
   // logd("Session::move: use_count={}", m_impl.use_count());
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

boost::asio::any_io_executor Session::get_executor() const noexcept { return impl->get_executor(); }

void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
{
   impl->async_submit(std::move(handler), "POST", url, std::move(headers));
}

// =================================================================================================

namespace
{

//
// Submits a request with a method of its own. The public Session::async_submit() is POST-only,
// but every backend can send whatever method it is handed, see Session::Impl::async_submit().
//
template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Submit) CompletionToken>
auto async_submit(Session::Impl& impl, std::string_view method, boost::urls::url url,
                  const Fields& headers, CompletionToken&& token)
{
   return asio::async_initiate<CompletionToken, Submit>(
      [&impl](auto&& handler, std::string_view method, boost::urls::url url,
              const Fields& headers) { //
         impl.async_submit(std::move(handler), method, std::move(url), headers);
      },
      token, method, std::move(url), headers);
}

//
// The whole of async_get(), as the coroutine it reads best as: submit, end the empty request
// body, wait for the response, read all of it. Every step is left to throw, and the co_spawn()
// below turns that back into the error code the caller gets.
//
// The implementation is held by shared_ptr: a Session released while the GET is still in flight
// must not pull the ground out from under the operations still running on it.
//
awaitable<client::Message> get_message(std::shared_ptr<Session::Impl> session, boost::urls::url url,
                                       Fields headers)
{
   //
   // A GET has no body, and saying so with a "Content-Length: 0" keeps HTTP/1.1 from framing one
   // as chunked -- which would leave the request incomplete until the write_eof() below, and the
   // session unable to take another one until then.
   //
   if (!headers.count(boost::beast::http::field::content_length) &&
       !headers.count(boost::beast::http::field::transfer_encoding))
      headers.set(boost::beast::http::field::content_length, "0");

   auto request = co_await async_submit(*session, "GET", std::move(url), headers, deferred);
   co_await request.async_write_eof();
   auto response = co_await request.async_get_response();

   client::Message message;
   message.result(static_cast<unsigned>(response.status_code()));
   for (auto&& field : response.fields())
      message.insert(field.name_string(), field.value()); // insert(), so repeated fields survive

   auto& body = message.body();
   std::array<char, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), as_tuple);
      body.append(buffer.data(), n);

      if (ec == asio::error::eof)
         break;
      else if (ec)
         throw boost::system::system_error(ec);
   }

   logd("async_get: {} {}, {} bytes", message.result_int(), message.reason(), body.size());
   co_return std::move(message);
}

} // namespace

void Session::async_get_any(GetHandler&& handler, boost::urls::url url, const Fields& headers)
{
   auto executor = get_associated_executor(handler, get_executor());
   auto slot = get_associated_cancellation_slot(handler);

   //
   // co_spawn() gives the coroutine a cancellation slot of its own, so binding the caller's to it
   // is what makes cancelling async_get() reach the operation it is currently waiting for.
   //
   co_spawn(get_executor(), get_message(impl, std::move(url), headers),
            bind_cancellation_slot(
               slot, bind_executor(executor, [handler = std::move(handler)](
                                                const std::exception_ptr& ep,
                                                client::Message message) mutable {
                  auto ec = code(ep);
                  if (ec)
                     message.result(boost::beast::http::status::unknown); // not the Beast default
                  std::move(handler)(ec, std::move(message));
               })));
}

// =================================================================================================

} // namespace anyhttp
