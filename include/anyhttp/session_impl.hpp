#pragma once

#include "session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>

namespace anyhttp
{

using Buffer = boost::beast::flat_buffer;

// =================================================================================================

class Session::Impl
{
public:
   virtual ~Impl() {}
   virtual void destroy(std::shared_ptr<Impl> self) = 0;

   virtual void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers) = 0;
   virtual asio::awaitable<void> do_session(Buffer&& data) = 0;
};

// =================================================================================================

} // namespace anyhttp
