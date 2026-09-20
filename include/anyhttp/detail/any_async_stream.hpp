#pragma once

//
// The type-erased async stream as its users see it. The implementation behind it is only forward
// declared here: it lives in anyhttp/detail/any_async_stream_impl.hpp, which src/
// any_async_stream_impl.cpp is the only place to include -- and to instantiate.
//

#include <anyhttp/buffer_array.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <memory>
#include <type_traits>

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
 * The socket underneath a stream, as far as the sessions care about it: enough for shutdown() and
 * close(), which is all they do with it. A TLS stream hands out its \c lowest_layer(), which is
 * this rather than the full ip::tcp::socket, so this is what the type-erased stream offers, too.
 */
using TcpSocketBase = asio::basic_socket<ip::tcp, asio::any_io_executor>;

/// The TLS stream the server and client run on, spelled out often enough to deserve a name.
using SslStream = asio::ssl::stream<ip::tcp::socket>;

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
class any_async_stream
{
public:
   using executor_type = boost::asio::any_io_executor;

   /// The type-erased stream itself, defined in anyhttp/detail/any_async_stream_impl.hpp.
   class Impl;

   explicit any_async_stream(std::unique_ptr<Impl> impl);
   any_async_stream(any_async_stream&&) noexcept;
   any_async_stream& operator=(any_async_stream&&) noexcept;
   ~any_async_stream();

   executor_type get_executor() noexcept;
   TcpSocketBase& get_socket();

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
   template <ConstBufferSequence Buffers,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite)
                CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
   auto async_write_some(const Buffers& buffers, CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, ConstBufferVector buffers) { //
            write_some(std::move(handler), std::move(buffers));
         },
         token, ConstBufferVector{buffers});
   }

   //
   // async_read_some
   //
   template <MutableBufferSequence Buffers,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite)
                CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
   auto async_read_some(const Buffers& buffers, CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, MutableBufferVector buffers) { //
            read_some(std::move(handler), std::move(buffers));
         },
         token, MutableBufferVector{buffers});
   }

   //
   // async_shutdown
   //
   // Ends the stream itself, which is something only a TLS stream has to do: see async_teardown()
   // in anyhttp/stream_traits.hpp, which is how the sessions reach this. For a stream that has
   // nothing to end, this completes immediately and successfully.
   //
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Shutdown)
                CompletionToken = asio::default_completion_token_t<asio::any_io_executor>>
   auto async_shutdown(CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, Shutdown>(initiate_shutdown{this}, token);
   }

private:
   //
   // A named initiation rather than a lambda, because it has to offer the executor the operation
   // runs on: tokens with a timer of their own -- cancel_after, which is how the sessions bound
   // the wait for the peer's "close_notify" -- look for it here.
   //
   struct initiate_shutdown
   {
      using executor_type = boost::asio::any_io_executor;
      executor_type get_executor() const noexcept { return self->get_executor(); }
      void operator()(ShutdownHandler handler) const { self->shutdown(std::move(handler)); }

      any_async_stream* self;
   };

   //
   // The initiations, with the buffer sequence already type-erased. Out of line, because this is
   // where the implementation is dereferenced -- it is incomplete here.
   //
   void write_some(ReadWriteHandler handler, ConstBufferVector buffers);
   void read_some(ReadWriteHandler handler, MutableBufferVector buffers);
   void shutdown(ShutdownHandler handler);

   std::unique_ptr<Impl> impl;
};

static_assert(boost::beast::is_async_stream<any_async_stream>::value);

// -------------------------------------------------------------------------------------------------

//
// The stream is moved into the type-erasing wrapper -- hence the rvalue reference, which the
// constraint keeps from matching an lvalue, just like the session factories do it.
//
// This is defined in anyhttp/detail/any_async_stream_impl.hpp and explicitly instantiated in
// src/any_async_stream_impl.cpp for each of the stream types below, so that the implementation
// is instantiated in that one place only.
//

template <typename Stream>
   requires(!std::is_reference_v<Stream>)
any_async_stream make_any_async_stream(Stream&& stream);

extern template any_async_stream make_any_async_stream<ip::tcp::socket>(ip::tcp::socket&&);
extern template any_async_stream make_any_async_stream<SslStream>(SslStream&&);

// =================================================================================================

} // namespace anyhttp
