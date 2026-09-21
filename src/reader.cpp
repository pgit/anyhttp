#include "anyhttp/reader.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

namespace anyhttp
{

// =================================================================================================

Reader::Reader(std::shared_ptr<Reader::Impl> impl) noexcept : m_impl(std::move(impl)) {}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Reader::~Reader() { reset(); }

void Reader::reset() noexcept
{
   if (m_impl)
   {
      m_impl->destroy();
      m_impl.reset();
   }
}

// -------------------------------------------------------------------------------------------------

Reader::executor_type Reader::get_executor() const noexcept
{
   return m_impl ? m_impl->get_executor() : executor_type{};
}

std::optional<size_t> Reader::content_length() const noexcept
{
   return m_impl ? m_impl->content_length() : std::nullopt;
}

void Reader::async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler)
{
   if (m_impl)
      m_impl->async_read_some(buffer, std::move(handler));
   else
      std::move(handler)(asio::error::bad_descriptor, 0);
}

// =================================================================================================

} // namespace anyhttp
