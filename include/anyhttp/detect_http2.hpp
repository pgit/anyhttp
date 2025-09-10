#pragma once
#include <boost/asio.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/co_composed.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/logic/tribool.hpp>

#include <cstring>
#include "anyhttp/common.hpp"

namespace anyhttp::server
{
namespace asio = boost::asio;

// =================================================================================================

namespace detail
{
template <typename T>
concept ConstBufferSequence = boost::asio::is_const_buffer_sequence<T>::value;

template <ConstBufferSequence ConstBufferSequence>
boost::tribool buffer_sequence_starts_with(const ConstBufferSequence& buffers,
                                           std::string_view prefix)
{
   if (prefix.empty()) // corner case
      return true;

   std::size_t matched = 0;
   const auto begin = boost::asio::buffer_sequence_begin(buffers);
   const auto end = boost::asio::buffer_sequence_end(buffers);
   for (auto it = begin; it != end; ++it)
   {
      const char* data = static_cast<const char*>(it->data());
      std::size_t size = it->size();

      std::size_t to_compare = std::min(size, prefix.size() - matched);
      if (std::memcmp(data, prefix.data() + matched, to_compare) != 0)
         return false;

      matched += to_compare;
      if (matched == prefix.size())
         return true;
   }

   return boost::indeterminate;
}

template <ConstBufferSequence ConstBufferSequence>
boost::tribool is_http2_client_preface(const ConstBufferSequence& buffers)
{
   // Make sure buffers meets the requirements
   static_assert(asio::is_const_buffer_sequence<ConstBufferSequence>::value,
                 "ConstBufferSequence type requirements not met");

   return buffer_sequence_starts_with(buffers, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
}

using boost::beast::detail::is_tls_client_hello;

} // namespace detail

// =================================================================================================

//
// https://www.boost.org/doc/libs/1_84_0/doc/html/boost_asio/reference/experimental__co_composed.html
//
template <typename AsyncReadStream, typename DynamicBuffer,
          typename CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
auto async_detect_http2_client_preface(AsyncReadStream& stream, DynamicBuffer& buffer,
                                       CompletionToken&& token = CompletionToken())
{
   static_assert(boost::beast::is_async_read_stream<AsyncReadStream>::value,
                 "AsyncReadStream type requirements not met");
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      experimental::co_composed<void(boost::system::error_code, bool)>(
         [](auto state, DynamicBuffer& buffer, AsyncReadStream& stream) -> void
         {
            //
            // https://think-async.com/Asio/asio-1.26.0/doc/asio/reference/experimental__co_composed.html
            //
            // With co_composed, per-operation cancellation is NOT enabled by default.
            //
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
                  co_return std::make_tuple(boost::system::error_code{}, static_cast<bool>(result));

               auto prepared = buffer.prepare(1460);
               auto [ec, n] = co_await stream.async_read_some(prepared, as_tuple);
               if (ec)
                  co_return {ec, false};
               buffer.commit(n);
            }
         },
         stream),
      token, std::ref(buffer), std::ref(stream));
}

// -------------------------------------------------------------------------------------------------

template <typename AsyncReadStream, typename DynamicBuffer,
          typename CompletionToken = DefaultCompletionToken>
auto async_detect_ssl_awaitable(AsyncReadStream& stream, DynamicBuffer& buffer,
                                CompletionToken&& token = CompletionToken())
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
         },
         stream),
      token, std::ref(stream), std::ref(buffer));
}

// =================================================================================================

} // namespace anyhttp::server
