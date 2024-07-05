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
   virtual void destroy() = 0;

   virtual void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) = 0;

   virtual asio::awaitable<void> do_server_session(Buffer&& data) = 0;
   virtual asio::awaitable<void> do_client_session(Buffer&& data) = 0;
};

// =================================================================================================

} // namespace anyhttp
