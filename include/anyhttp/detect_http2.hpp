#pragma once
#include <boost/asio.hpp>
#include <boost/asio/experimental/co_composed.hpp>
#include <boost/logic/tribool.hpp>

#include <cstring>

namespace anyhttp::server
{
namespace asio = boost::asio;

// =================================================================================================

namespace detail
{
template <class ConstBufferSequence>
boost::tribool is_http2_client_preface(ConstBufferSequence const& buffers)
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
} // namespace detail

// =================================================================================================

//
// https://www.boost.org/doc/libs/1_84_0/doc/html/boost_asio/reference/experimental__co_composed.html
//
template <typename AsyncReadStream, typename DynamicBuffer, typename CompletionToken>
auto async_detect_http2_client_preface(AsyncReadStream& stream, DynamicBuffer& buffer,
                                       CompletionToken&& token)
{
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      experimental::co_composed<void(boost::system::error_code, bool)>(
         [](auto state, AsyncReadStream& stream, DynamicBuffer& buffer) -> void
         {
            try
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
                  size_t n = co_await stream.async_read_some(prepared, deferred);
                  buffer.commit(n);
               }
            }
            catch (const boost::system::system_error& e)
            {
               co_return {e.code(), false};
            }
         },
         stream),
      token, std::ref(stream), std::ref(buffer));
}

// =================================================================================================

} // namespace anyhttp::server