#pragma once

#include "session.hpp"

#include <boost/asio/awaitable.hpp>

namespace anyhttp
{

// =================================================================================================

class Session::Impl
{
public:
   virtual ~Impl() {}
   virtual void destroy() = 0;

   virtual void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) = 0;

   virtual asio::awaitable<void> do_server_session(std::vector<uint8_t> data) = 0;
   virtual asio::awaitable<void> do_client_session(std::vector<uint8_t> data) = 0;
};

// =================================================================================================

} // namespace anyhttp
