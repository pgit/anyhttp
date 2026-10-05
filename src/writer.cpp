#include "anyhttp/writer_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

namespace anyhttp
{

// =================================================================================================

Writer::Writer(std::shared_ptr<Writer::Impl> impl) noexcept : impl_(std::move(impl)) {}

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

Writer::~Writer() { reset(); }

void Writer::reset() noexcept
{
   if (impl_)
   {
      impl_->destroy();
      impl_.reset();
   }
}

// -------------------------------------------------------------------------------------------------

Writer::executor_type Writer::get_executor() const noexcept
{
   return impl_ ? impl_->get_executor() : executor_type{};
}

std::string Writer::log_prefix() const { return impl_ ? impl_->log_prefix() : std::string{}; }

void Writer::content_length(std::optional<size_t> content_length)
{
   return impl_ ? impl_->content_length(content_length) : void{};
}

static Task<std::tuple<error_code>> no_writer() { co_return std::tuple{errors::bad_descriptor}; }

Task<std::tuple<error_code>> Writer::write(asio::const_buffer buffer)
{
   return impl_ ? impl_->write(buffer, false) : no_writer();
}

Task<std::tuple<error_code>> Writer::write_eof(asio::const_buffer buffer)
{
   return impl_ ? impl_->write(buffer, true) : no_writer();
}

#if ANYHTTP_ASIO
void Writer::async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof)
{
   if (impl_)
      impl_->async_write(std::move(handler), buffer, eof);
   else
      std::move(handler)(errors::bad_descriptor);
}
#endif

// =================================================================================================

} // namespace anyhttp
