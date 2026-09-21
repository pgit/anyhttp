#include "anyhttp/writer_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

namespace anyhttp
{

// =================================================================================================

Writer::Writer(std::shared_ptr<Writer::Impl> impl) noexcept : m_impl(std::move(impl)) {}

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

Writer::~Writer() { reset(); }

void Writer::reset() noexcept
{
   if (m_impl)
   {
      m_impl->destroy();
      m_impl.reset();
   }
}

// -------------------------------------------------------------------------------------------------

Writer::executor_type Writer::get_executor() const noexcept
{
   return m_impl ? m_impl->get_executor() : executor_type{};
}

void Writer::content_length(std::optional<size_t> content_length)
{
   assert(m_impl);
   m_impl->content_length(content_length);
}

void Writer::async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof)
{
   if (m_impl)
      m_impl->async_write(std::move(handler), buffer, eof);
   else
      std::move(handler)(asio::error::bad_descriptor);
}

// =================================================================================================

} // namespace anyhttp
