#pragma once

#include <span>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <range/v3/view/any_view.hpp>

namespace asio = boost::asio;
namespace ip = asio::ip;

namespace anyhttp
{
// =================================================================================================

using ReadWrite = void(boost::system::error_code, std::size_t);
using ReadWriteHandler = asio::any_completion_handler<ReadWrite>;

using ConstBufferSpan = std::span<const asio::const_buffer>;
using MutableBufferSpan = std::span<asio::mutable_buffer>;

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
      virtual void async_write_impl(ReadWriteHandler handler, ConstBufferSpan buffer) = 0;
      virtual void async_read_impl(ReadWriteHandler handler, MutableBufferSpan buffer) = 0;
   };

public:
   std::unique_ptr<Impl> impl;
   AnyAsyncStream(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}

   inline executor_type get_executor() noexcept { return impl->get_executor(); }
   inline ip::tcp::socket& get_socket() { return impl->get_socket(); }

   //
   // async_write_some
   //
   template <typename ConstBufferSequence,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadWrite) CompletionToken>
      requires boost::beast::is_const_buffer_sequence<ConstBufferSequence>::value
   auto async_write_some(const ConstBufferSequence& buffers, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, ReadWrite>(
         [this](ReadWriteHandler handler, const ConstBufferSequence& buffers)
         {
            size_t size = std::distance(asio::buffer_sequence_begin(buffers),
                                        asio::buffer_sequence_end(buffers));
            std::vector<asio::const_buffer> temp;
            temp.reserve(size);
            std::copy(asio::buffer_sequence_begin(buffers), asio::buffer_sequence_end(buffers),
                      std::back_inserter(temp));
            auto span = ConstBufferSpan(temp.begin(), size);
            impl->async_write_impl([handler = std::move(handler), temp = std::move(temp)](
                                      boost::system::error_code ec, size_t n) mutable
                                   { std::move(handler)(ec, n); },
                                   span);
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
#if 0
            auto range = ranges::subrange(asio::buffer_sequence_begin(buffers),
                                          asio::buffer_sequence_end(buffers));
            ranges::any_view<asio::mutable_buffer> anyrange(range);
            impl->async_read_impl(std::move(handler), anyrange);
#else
            size_t size = std::distance(asio::buffer_sequence_begin(buffers),
                                        asio::buffer_sequence_end(buffers));
            //
            // FIXME: allocating a vector is slow -- but we are passing a std::span<>, which
            //        contains the address of one (or more) buffers. And that address needs
            //        to be stored somewhere for the duration of the asyn op. Maybe it is not
            //        the best idea to use spans here...
            //
            std::vector<asio::mutable_buffer> temp;
            temp.reserve(size);
            std::copy(asio::buffer_sequence_begin(buffers), asio::buffer_sequence_end(buffers),
                      std::back_inserter(temp));
            auto span = MutableBufferSpan(temp.begin(), size);
            impl->async_read_impl([handler = std::move(handler), temp = std::move(temp)](
                                     boost::system::error_code ec, size_t n) mutable
                                  { std::move(handler)(ec, n); },
                                  span);
#endif
         },
         token, buffers);
   }
};

static_assert(boost::beast::is_async_stream<AnyAsyncStream>::value);

// =================================================================================================

} // namespace anyhttp
