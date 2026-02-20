#include "anyhttp/session.hpp"
#include "anyhttp/session_impl.hpp"

using namespace boost::asio;

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : impl(std::move(impl))
{
   // logd("Session::ctor: use_count={}", m_impl.use_count());
}

// -------------------------------------------------------------------------------------------------

Session::Session(Session&& other) noexcept : impl(std::move(other.impl))
{
   // logd("Session::move: use_count={}", m_impl.use_count());
}

Session& Session::operator=(Session&& other) noexcept
{
   if (this != &other)
   {
      impl = std::move(other.impl);
   }
   logd("Session::move: use_count={}", impl.use_count());
   return *this;
}

void Session::reset() noexcept
{
   if (impl)
   {
      impl->destroy();
      impl.reset();
   }
}

Session::~Session() { reset(); }

// -------------------------------------------------------------------------------------------------

boost::asio::any_io_executor Session::get_executor() const noexcept
{
   return impl->get_executor();
}

void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
{
   impl->async_submit(std::move(handler), url, std::move(headers));
}

// =================================================================================================

} // namespace anyhttp
