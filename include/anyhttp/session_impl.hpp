#pragma once

#include "session.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <memory>

namespace anyhttp
{

using Buffer = boost::beast::flat_buffer;

// =================================================================================================

class Session::Impl : public std::enable_shared_from_this<Session::Impl>
{
public:
   virtual ~Impl() {}
   virtual boost::asio::any_io_executor get_executor() const noexcept = 0;
   virtual void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers) = 0;
   virtual asio::awaitable<void> do_session(Buffer&& data) = 0;
   virtual void destroy() noexcept = 0;
};

// =================================================================================================

} // namespace anyhttp
