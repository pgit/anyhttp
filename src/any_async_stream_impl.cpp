#include "anyhttp/detail/any_async_stream_impl.hpp"

namespace anyhttp
{

// =================================================================================================

//
// Everything that has to see the implementation, which is incomplete in the header the users of
// any_async_stream include.
//

any_async_stream::any_async_stream(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}
any_async_stream::any_async_stream(any_async_stream&&) noexcept = default;
any_async_stream& any_async_stream::operator=(any_async_stream&&) noexcept = default;
any_async_stream::~any_async_stream() = default;

any_async_stream::executor_type any_async_stream::get_executor() noexcept
{
   return impl->get_executor();
}

TcpSocketBase& any_async_stream::get_socket() { return impl->get_socket(); }

void any_async_stream::write_some(ReadWriteHandler handler, ConstBufferVector buffers)
{
   impl->async_write_some(std::move(handler), std::move(buffers));
}

void any_async_stream::read_some(ReadWriteHandler handler, MutableBufferVector buffers)
{
   impl->async_read_some(std::move(handler), std::move(buffers));
}

// =================================================================================================
// The implementations, see anyhttp/detail/any_async_stream_impl.hpp. Instantiating them is kept to
// this translation unit, so that including the type-erased stream stays cheap.
// =================================================================================================

template class any_async_stream_impl<ip::tcp::socket>;
template class any_async_stream_impl<SslStream>;

template any_async_stream make_any_async_stream<ip::tcp::socket>(ip::tcp::socket&&);
template any_async_stream make_any_async_stream<SslStream>(SslStream&&);

// =================================================================================================

} // namespace anyhttp
