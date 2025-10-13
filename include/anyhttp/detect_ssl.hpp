#pragma once
#include <boost/asio.hpp>
#include <boost/asio/experimental/co_composed.hpp>

#include <boost/beast/core/detect_ssl.hpp>

#include <boost/logic/tribool.hpp>

namespace anyhttp::server
{
namespace asio = boost::asio;

// =================================================================================================

namespace detail
{

using boost::beast::detail::is_tls_client_hello;

} // namespace detail

// =================================================================================================

template <typename AsyncReadStream, typename DynamicBuffer,
          typename CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
auto async_detect_ssl_awaitable(AsyncReadStream& stream, DynamicBuffer& buffer,
                                CompletionToken&& token = CompletionToken())
{
   static_assert(boost::beast::is_async_read_stream<AsyncReadStream>::value,
                 "AsyncReadStream type requirements not met");
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      co_composed<void(boost::system::error_code, bool)>(
         [](auto state, AsyncReadStream& stream, DynamicBuffer& buffer) -> void
   {
      state.reset_cancellation_state(enable_terminal_cancellation());

      for (;;)
      {
         boost::tribool result = detail::is_tls_client_hello(buffer.data());
         if (!boost::indeterminate(result))
            co_return std::make_tuple(boost::system::error_code{}, static_cast<bool>(result));

         auto prepared = buffer.prepare(1460);
         auto [ec, n] = co_await stream.async_read_some(prepared, as_tuple);
         if (ec)
            co_return {ec, false};
         buffer.commit(n);
      }
   }, stream),
      token, std::ref(stream), std::ref(buffer));
}

// =================================================================================================

} // namespace anyhttp::server
