#pragma once

#include <anyhttp/buffer_array.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/immediate.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/container/small_vector.hpp>

#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/stream_traits.hpp>

namespace asio = boost::asio;
namespace ip = asio::ip;

namespace anyhttp
{

// =================================================================================================

using ReadWrite = void(boost::system::error_code, std::size_t);
using ReadWriteHandler = asio::any_completion_handler<ReadWrite>;

using ConstBufferVector = const_buffer_array<16>;
using MutableBufferVector = mutable_buffer_array<16>;

using Shutdown = void(boost::system::error_code);
using ShutdownHandler = asio::any_completion_handler<Shutdown>;

/**
 * Attempt to create a type-erased async stream with ASIO.
 *
 * The difficult part here is to type-erase the buffer sequences. The buffers are copied into a
 * buffer_array, a fixed-capacity, non-allocating array of buffer descriptors that is itself a
 * buffer sequence. This seems to work reasonably well.
 *
 * There is also \c asio::buffer_sequence_adapter and \c linearise(), which seem to be used in ASIO
 * SSL code as well. It merges a set of buffers into a new, contiguous buffer. But that is slow.
 */
class AnyAsyncStream
{
public:
   using executor_type = boost::asio::any_io_executor;

   class Impl
   {
   public:
      virtual ~Impl() = default;

      using executor_type = boost::asio::any_io_executor;
      virtual executor_type get_executor() noexcept = 0;
      virtual ip::tcp::socket& get_socket() = 0;

      virtual void async_write_some(ReadWriteHandler handler, ConstBufferVector buffer) = 0;
      virtual void async_read_some(ReadWriteHandler handler, MutableBufferVector buffer) = 0;
      virtual void async_shutdown_impl(ShutdownHandler handler)
      {
         auto ex = boost::asio::get_associated_immediate_executor(handler, get_executor());
         ex.execute([handler = std::move(handler)]() mutable { //
            std::move(handler)(boost::system::error_code());
         });
      }
   };

protected:
   std::unique_ptr<Impl> impl;

public:
   AnyAsyncStream(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}

   executor_type get_executor() noexcept { return impl->get_executor(); }
   ip::tcp::socket& get_socket() { return impl->get_socket(); }

   //
   // async_write_some
   //
   // The async operations of ASIO are designed to work with sequences of buffers. Those cannot
   // easily be type-erased, so we copy the buffer descriptors into a fixed-capacity array.
   //
   // The requirements for ConstBufferSequence are defined here:
   // https://live.boost.org/doc/libs/1_88_0/doc/html/boost_asio/reference/ConstBufferSequence.html
   //
   // The iterators returned by asio::buffer_sequence_{begin,end} must be 'bidirectional', but are
   // not required to be 'contiguous'. So those iterators cannot be simply converted to a span.
   //
   // * https://en.cppreference.com/w/cpp/iterator/bidirectional_iterator
   // * https://en.cppreference.com/w/cpp/iterator/contiguous_iterator.html
   //
   // Instead, we copy them into a buffer_array, which is itself a (contiguous) buffer sequence and
   // can be passed on to the underlying stream unchanged. Nothing is merged or linearized, so
   // scatter/gather I/O is preserved. Empty buffers are dropped while copying, and sequences longer
   // than the array's capacity are truncated -- which is harmless for a "some" operation, as it
   // just results in a shorter transfer.
   //
   template <typename ConstBufferSequence,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite)
                CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
      requires boost::beast::is_const_buffer_sequence<ConstBufferSequence>::value
   auto async_write_some(const ConstBufferSequence& buffers,
                         CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, const ConstBufferSequence& buffers)
      {  //
         impl->async_write_some(std::move(handler), ConstBufferVector{buffers});
      }, token, buffers);
   }

   //
   // async_read_some
   //
   template <typename MutableBufferSequence,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite)
                CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
      requires boost::beast::is_mutable_buffer_sequence<MutableBufferSequence>::value
   auto async_read_some(const MutableBufferSequence& buffers,
                        CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, const MutableBufferSequence& buffers)
      {  //
         impl->async_read_some(std::move(handler), MutableBufferVector{buffers});
      }, token, buffers);
   }
};

static_assert(boost::beast::is_async_stream<AnyAsyncStream>::value);

// =================================================================================================

} // namespace anyhttp
