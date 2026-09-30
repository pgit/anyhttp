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
   virtual Executor get_executor() const noexcept = 0;
   virtual void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                             const Fields& headers) = 0;

   /// async_submit() as a coroutine, see Reader::Impl::read_some().
   virtual Task<std::tuple<error_code, client::Request>>
   submit(std::string method, boost::urls::url url, Fields headers)
   {
      co_return co_await initiate<Submit>([&](SubmitHandler handler) {
         async_submit(std::move(handler), method, std::move(url), headers);
      });
   }
   virtual Task<void> do_session(Buffer&& data) = 0;
   virtual void destroy() noexcept = 0;
};

// =================================================================================================

} // namespace anyhttp
