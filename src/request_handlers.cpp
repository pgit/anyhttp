#include "anyhttp/request_handlers.hpp"
#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <boost/asio/deferred.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fmt/ostream.h>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;

namespace anyhttp
{

// =================================================================================================

awaitable<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   co_await response.async_submit(200, {}, deferred);
   for (;;)
   {
      auto buffer = co_await request.async_read_some(deferred);
      co_await response.async_write(asio::buffer(buffer), deferred);
      if (buffer.empty())
         co_return;
   }
}

awaitable<void> echo_old(Request request, Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());
   co_await response.async_submit(200, {}, deferred);
   try
   {
      for (;;)
      {
         logd("async_read_some...");
         auto buffer = co_await request.async_read_some(deferred);
         logd("async_read_some... done, read {} bytes", buffer.size());

#if 0
         auto timer = steady_timer(request.executor());
         timer.expires_after(1ms);
         logd("sleep...");
         co_await timer.async_wait(deferred);
         logd("sleep... done");
#endif

         logd("async_write...");
         co_await response.async_write(asio::buffer(buffer), deferred);

         if (!buffer.empty())
            logd("async_write... done, wrote {} bytes", buffer.size());
         else
         {
            logd("async_write... done, wrote {} bytes, finished", buffer.size());
            break;
         }
      }
   }
   catch (std::exception& ex)
   {
      logw("exception: {}", ex.what());
   }
   co_return;
}

awaitable<void> not_found(server::Request request, server::Response response)
{
   co_await response.async_submit(404, {}, deferred);
   co_await response.async_write({}, deferred);
}

awaitable<void> eat_request(server::Request request, server::Response response)
{
   co_await response.async_submit(200, {}, deferred);
   co_await response.async_write({}, deferred);
   for (;;)
   {
      auto buffer = co_await request.async_read_some(deferred);
      if (buffer.empty())
         break;
   }

   co_await anyhttp::sleep(100ms);
}

awaitable<void> delayed(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

awaitable<void> detach(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes)
{
   if (bytes == 0)
      co_return co_await request.async_write({}, deferred);

   std::vector<uint8_t> buffer;
   buffer.resize(bytes);
   try
   {
      co_await request.async_write(asio::buffer(buffer), deferred);
      logi("send: done, wrote {} bytes", bytes);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("send: {}", ec.what());
   }
   co_await request.async_write({}, deferred);
}

awaitable<size_t> receive(client::Response& response)
{
   size_t bytes = 0;
   for (;;)
   {
      auto buf = co_await response.async_read_some(deferred);
      if (buf.empty())
         break;

      logd("receive: {}", buf.size());
      bytes += buf.size();
   }

   logi("receive: EOF after reading {} bytes", bytes);
   co_return bytes;
}

awaitable<size_t> read_response(client::Request& request)
{
   auto response = co_await request.async_get_response(asio::deferred);
   co_return co_await receive(response);
}

awaitable<void> sendEOF(client::Request& request)
{
   logi("send: finishing request...");
   co_await request.async_write({}, deferred);
   logi("send: finishing request... done");
}

// =================================================================================================

} // namespace anyhttp