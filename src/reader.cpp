#include "anyhttp/reader_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

namespace anyhttp
{

// =================================================================================================

Reader::Reader(std::shared_ptr<Reader::Impl> impl) noexcept : impl_(std::move(impl)) {}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Reader::~Reader() { reset(); }

void Reader::reset() noexcept
{
   if (impl_)
   {
      impl_->destroy();
      impl_.reset();
   }
}

// -------------------------------------------------------------------------------------------------

Reader::executor_type Reader::get_executor() const noexcept
{
   return impl_ ? impl_->get_executor() : executor_type{};
}

std::string Reader::log_prefix() const { return impl_ ? impl_->log_prefix() : std::string{}; }

std::optional<size_t> Reader::content_length() const noexcept
{
   return impl_ ? impl_->content_length() : std::nullopt;
}

void Reader::async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler)
{
   if (impl_)
      impl_->async_read_some(buffer, std::move(handler));
   else
      std::move(handler)(errors::bad_descriptor, 0);
}

// =================================================================================================

} // namespace anyhttp
