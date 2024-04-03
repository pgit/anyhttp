#include "anyhttp/session.hpp"
#include "anyhttp/session_impl.hpp"

namespace anyhttp
{

// =================================================================================================

Session::Session(std::shared_ptr<Session::Impl> impl) : m_impl(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session::Session(const Session&) = default;
Session::~Session() = default;

void Session::async_submit_any(SubmitHandler&& handler, boost::urls::url url, Fields headers)
{
   m_impl->async_submit(std::move(handler), url, std::move(headers));
}

// =================================================================================================

} // namespace anyhttp
