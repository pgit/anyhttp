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
   virtual client::Request submit(boost::urls::url url, Fields headers) = 0;

   virtual void cancel() = 0;
   virtual asio::awaitable<void> do_server_session(std::vector<uint8_t> data) = 0;
   virtual asio::awaitable<void> do_client_session(std::vector<uint8_t> data) = 0;
};

// =================================================================================================

} // namespace anyhttp
