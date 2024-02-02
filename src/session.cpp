#include "anyhttp/session.hpp"
#include "anyhttp/session_impl.hpp"

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : m_impl(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session::~Session() = default;

client::Request Session::submit(boost::urls::url url, Fields headers)
{
   return m_impl->submit(std::move(url), std::move(headers));
}

// =================================================================================================

} // namespace anyhttp
