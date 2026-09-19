#pragma once

//
// Definition of the implementation behind anyhttp::any_async_stream, instantiated only by
// src/any_async_stream_impl.cpp -- make_any_async_stream() is what everyone else uses instead.
//

#include "anyhttp/detail/any_async_stream.hpp"
#include "anyhttp/stream_traits.hpp"

#include <boost/asio/associated_immediate_executor.hpp>

#include <utility>

namespace anyhttp
{

// =================================================================================================

/**
 * The type-erased stream, with the buffer sequences of the async operations erased as well: they
 * arrive as a \c buffer_array, which is a buffer sequence itself and can be handed to the
 * underlying stream unchanged.
 */
class any_async_stream::Impl
{
public:
   virtual ~Impl() = default;

   using executor_type = boost::asio::any_io_executor;
   virtual executor_type get_executor() noexcept = 0;
   virtual TcpSocketBase& get_socket() = 0;

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

// -------------------------------------------------------------------------------------------------

/**
 * The implementation for a concrete stream, which is moved in and owned here -- among other things
 * so that the socket underneath it stays around for cancellation. Everything beyond the async read
 * and write operations comes from \c stream_traits, so this works for every stream a session can
 * run on.
 */
template <SocketStream Stream>
class any_async_stream_impl final : public any_async_stream::Impl
{
public:
   explicit any_async_stream_impl(Stream stream) : m_stream(std::move(stream)) {}

   executor_type get_executor() noexcept override
   {
      return stream_traits<Stream>::get_executor(m_stream);
   }
   TcpSocketBase& get_socket() override { return anyhttp::get_socket(m_stream); }

   void async_write_some(ReadWriteHandler handler, ConstBufferVector buffers) override
   {
      m_stream.async_write_some(buffers, std::move(handler));
   }

   void async_read_some(ReadWriteHandler handler, MutableBufferVector buffers) override
   {
      m_stream.async_read_some(buffers, std::move(handler));
   }

private:
   Stream m_stream;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
   requires(!std::is_reference_v<Stream>)
any_async_stream make_any_async_stream(Stream&& stream)
{
   return any_async_stream(std::make_unique<any_async_stream_impl<Stream>>(std::move(stream)));
}

extern template class any_async_stream_impl<ip::tcp::socket>;
extern template class any_async_stream_impl<SslStream>;

// =================================================================================================

} // namespace anyhttp
