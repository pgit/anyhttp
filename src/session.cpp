#include "anyhttp/session.hpp"
#include "anyhttp/session_impl.hpp"

using namespace boost::asio;

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : m_impl(std::move(impl))
{
   // logd("Session::ctor: use_count={}", m_impl.use_count());
}

// -------------------------------------------------------------------------------------------------

Session::Session(Session&& other) noexcept : m_impl(std::move(other.m_impl))
{
   // logd("Session::move: use_count={}", m_impl.use_count());
}

Session& Session::operator=(Session&& other) noexcept
{
   if (this != &other)
   {
      m_impl = std::move(other.m_impl);
   }
   logd("Session::move: use_count={}", m_impl.use_count());
   return *this;
}

void Session::reset() noexcept
{
   if (m_impl)
   {
      auto temp = m_impl.get();
      temp->destroy(std::move(m_impl));
   }
}

Session::~Session() { reset(); }

// -------------------------------------------------------------------------------------------------

boost::asio::any_io_executor Session::get_executor() const noexcept
{
   return m_impl->get_executor();
}

void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
{
   m_impl->async_submit(std::move(handler), url, std::move(headers));
}

// =================================================================================================

} // namespace anyhttp
