#pragma once
#include <boost/asio.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/co_composed.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/logic/tribool.hpp>

#include <cstring>
#include <iostream>

namespace anyhttp::server
{
namespace asio = boost::asio;

// =================================================================================================

namespace detail
{
template <class ConstBufferSequence>
boost::tribool is_http2_client_preface(const ConstBufferSequence& buffers)
{
   // Make sure buffers meets the requirements
   static_assert(asio::is_const_buffer_sequence<ConstBufferSequence>::value,
                 "ConstBufferSequence type requirements not met");

   // Flatten the input buffers into a single contiguous range
   // of bytes on the stack to make it easier to work with the data.
   std::array<char, 24> buf;
   auto const n = asio::buffer_copy(asio::mutable_buffer(buf.data(), buf.size()), buffers);

   if (strncmp("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", buf.data(), n))
      return false;

   if (n < 24)
      return boost::indeterminate;

   return true;
}

using boost::beast::detail::is_tls_client_hello;

} // namespace detail

// =================================================================================================

//
// https://www.boost.org/doc/libs/1_84_0/doc/html/boost_asio/reference/experimental__co_composed.html
//
template <typename AsyncReadStream, typename DynamicBuffer, typename CompletionToken>
auto async_detect_http2_client_preface(AsyncReadStream& stream, DynamicBuffer& buffer,
                                       CompletionToken&& token)
{
   static_assert(boost::beast::is_async_read_stream<AsyncReadStream>::value,
                 "AsyncReadStream type requirements not met");
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      experimental::co_composed<void(boost::system::error_code, bool)>(
         [](auto state, AsyncReadStream& stream, DynamicBuffer& buffer) -> void
         {
            state.throw_if_cancelled(true);
            state.reset_cancellation_state(enable_terminal_cancellation());

            //
            // FIXME: As a coroutine, we don't even need to loop like this.
            //        Instead, we could just read the minimum number of bytes needed for
            //        detection in one async call to async_read(). But we keep it like this for
            //        now, leaving it up to the detection function (is_http2_client_preface) to
            //        request as much additional buffer as it needs, dynamically.
            //
            for (;;)
            {
               boost::tribool result = detail::is_http2_client_preface(buffer.data());
               if (!boost::indeterminate(result))
                  co_return {{}, static_cast<bool>(result)};

               auto prepared = buffer.prepare(1460);
               auto [ec, n] = co_await stream.async_read_some(prepared, as_tuple(deferred));
               if (ec)
                  co_return {ec, false};
               buffer.commit(n);
            }
         },
         stream),
      token, std::ref(stream), std::ref(buffer));
}

// -------------------------------------------------------------------------------------------------

template <typename AsyncReadStream, typename DynamicBuffer, typename CompletionToken>
auto async_detect_ssl_awaitable(AsyncReadStream& stream, DynamicBuffer& buffer,
                                CompletionToken&& token)
{
   static_assert(boost::beast::is_async_read_stream<AsyncReadStream>::value,
                 "AsyncReadStream type requirements not met");
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      experimental::co_composed<void(boost::system::error_code, bool)>(
         [](auto state, AsyncReadStream& stream, DynamicBuffer& buffer) -> void
         {
            state.throw_if_cancelled(false);
            state.reset_cancellation_state(enable_terminal_cancellation());

            for (;;)
            {
               boost::tribool result = detail::is_tls_client_hello(buffer.data());
               if (!boost::indeterminate(result))
                  co_return {{}, static_cast<bool>(result)};

               auto prepared = buffer.prepare(1460);
               auto [ec, n] = co_await stream.async_read_some(prepared, as_tuple(deferred));
               if (ec)
                  co_return {ec, false};
               buffer.commit(n);
            }
         },
         stream),
      token, std::ref(stream), std::ref(buffer));
}

// =================================================================================================

} // namespace anyhttp::server
