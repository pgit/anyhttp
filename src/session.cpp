#include "anyhttp/session.hpp"
#include "anyhttp/session_impl.hpp"

using namespace boost::asio;

namespace anyhttp
{

// =================================================================================================

class SessionWrapper
{
public:
   SessionWrapper(std::shared_ptr<Session::Impl> impl) : impl(impl)
   {
      logw("SessionWrapper: ctor");
   }

   ~SessionWrapper()
   {
      logw("SessionWrapper: dtor");
      impl->destroy();
   }

   std::shared_ptr<Session::Impl> impl;
};

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

// -------------------------------------------------------------------------------------------------

Session::~Session()
{
   if (m_impl)
   {
      m_impl->destroy();
      m_impl.reset();
      logd("Session::dtor: use_count={}", m_impl.use_count());
   }
}

void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, Fields headers)
{
   m_impl->async_submit(std::move(handler), url, std::move(headers));
}

// =================================================================================================

} // namespace anyhttp
