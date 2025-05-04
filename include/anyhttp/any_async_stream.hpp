#pragma once

#include <span>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/container/small_vector.hpp>

#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <range/v3/view/any_view.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

namespace asio = boost::asio;
namespace ip = asio::ip;

namespace anyhttp
{
// =================================================================================================

using ReadWrite = void(boost::system::error_code, std::size_t);
using ReadWriteHandler = asio::any_completion_handler<ReadWrite>;

#if 0
using ConstBufferVector = std::vector<asio::const_buffer>;
using MutableBufferVector = std::vector<asio::mutable_buffer>;
#else
using ConstBufferVector = boost::container::small_vector<asio::const_buffer, 4>;
using MutableBufferVector = boost::container::small_vector<asio::mutable_buffer, 4>;
#endif

/**
 * Attempt to create a type-erased async stream.
 */
class AnyAsyncStream
{
public:
   using executor_type = boost::asio::any_io_executor;

   class Impl
   {
   public:
      using executor_type = boost::asio::any_io_executor;
      virtual ~Impl() = default;
      virtual executor_type get_executor() noexcept = 0;
      virtual ip::tcp::socket& get_socket() = 0;
      virtual void async_write_impl(ReadWriteHandler handler, ConstBufferVector buffer) = 0;
      virtual void async_read_impl(ReadWriteHandler handler, MutableBufferVector buffer) = 0;
   };

public:
   std::unique_ptr<Impl> impl;
   AnyAsyncStream(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}

   inline executor_type get_executor() noexcept { return impl->get_executor(); }
   inline ip::tcp::socket& get_socket() { return impl->get_socket(); }

   //
   // async_write_some
   //
   // The async operations of ASIO are designed to work with sequences of buffers. Those cannot
   // easily be type-erased, aside transforming them to a vector.
   //
   // The requirements for ConstBufferSequence are defined here:
   // https://live.boost.org/doc/libs/1_88_0/doc/html/boost_asio/reference/ConstBufferSequence.html
   //
   // The iterators returned by asio::buffer_sequence_{begin,end} must model:
   // * https://en.cppreference.com/w/cpp/iterator/bidirectional_iterator
   //
   // So those iterators cannot be simply modelled as a span.
   //
   template <typename ConstBufferSequence,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite) CompletionToken>
      requires boost::beast::is_const_buffer_sequence<ConstBufferSequence>::value
   auto async_write_some(const ConstBufferSequence& buffers, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, const ConstBufferSequence& buffers)
         {
            impl->async_write_impl(std::move(handler),
                                   ConstBufferVector{asio::buffer_sequence_begin(buffers),
                                                     asio::buffer_sequence_end(buffers)});
         },
         token, buffers);
   }

   //
   // async_read_some
   //
   template <typename MutableBufferSequence,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite) CompletionToken>
      requires boost::beast::is_mutable_buffer_sequence<MutableBufferSequence>::value
   auto async_read_some(const MutableBufferSequence& buffers, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, const MutableBufferSequence& buffers)
         {
            impl->async_read_impl(std::move(handler),
                                  MutableBufferVector{asio::buffer_sequence_begin(buffers),
                                                      asio::buffer_sequence_end(buffers)});
         },
         token, buffers);
   }
};

static_assert(boost::beast::is_async_stream<AnyAsyncStream>::value);

// =================================================================================================

} // namespace anyhttp
