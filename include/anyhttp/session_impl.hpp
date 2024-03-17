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

   /**
    * Submits a new request.
    *
    * Submitting a request means initiating the sending of the request method, path and any headers.
    * It does not mean that all or even any of those are actually transmitted immediatelly. The same
    * is true if you start an asynchronous write operation on the request object. The request may
    * just be queued for later transmission.
    * 
    * Use \ref Request::async_get_response() to wait for the response.
    */    
   virtual client::Request submit(boost::urls::url url, Fields headers) = 0;

   virtual void cancel() = 0;
   virtual asio::awaitable<void> do_server_session(std::vector<uint8_t> data) = 0;
   virtual asio::awaitable<void> do_client_session(std::vector<uint8_t> data) = 0;
};

// =================================================================================================

} // namespace anyhttp
