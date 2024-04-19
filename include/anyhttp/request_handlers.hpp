#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>

#include <fmt/ostream.h>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/chunk.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

namespace anyhttp
{

// =================================================================================================

template <typename T>
boost::asio::awaitable<void> sleep(T duration)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(duration);
   try
   {
      co_await timer.async_wait(asio::deferred);
      logi("sleep: done");
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sleep: {}", ec.what());
   }
}

boost::asio::awaitable<void> echo(server::Request request, server::Response response);
boost::asio::awaitable<void> not_found(server::Request request, server::Response response);
boost::asio::awaitable<void> eat_request(server::Request request, server::Response response);

boost::asio::awaitable<void> delayed(server::Request request, server::Response response);
boost::asio::awaitable<void> detach(server::Request request, server::Response response);

// =================================================================================================

boost::asio::awaitable<void> send(client::Request& request, size_t bytes);
boost::asio::awaitable<size_t> receive(client::Response& response);
boost::asio::awaitable<size_t> read_response(client::Request& request);
boost::asio::awaitable<void> sendEOF(client::Request& request);

// =================================================================================================

template <typename Range>
   requires ranges::contiguous_range<Range> && ranges::borrowed_range<Range>
boost::asio::awaitable<void> send(client::Request& request, Range range)
{
   co_await request.async_write(asio::buffer(range.data(), range.size()), boost::asio::deferred);
   co_await request.async_write({}, asio::deferred);
   logi("send: done");
}

template <typename Range>
   requires ranges::borrowed_range<Range> && (!ranges::contiguous_range<Range>)
boost::asio::awaitable<void> send(client::Request& request, Range range, bool eof = true)
{
   size_t bytes = 0;
   // std::array<uint8_t, 1460> buffer;
   std::array<uint8_t, 16 * 1024> buffer;
   for (auto chunk : range | ranges::views::chunk(buffer.size()))
   {
      auto end = std::ranges::copy(chunk, buffer.data()).out;
      bytes += end - buffer.data();
#if 0
      auto result = co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                                 as_tuple(deferred));
      if (auto ec = std::get<0>(result))
      {
         loge("send: (range) {}", ec.what());
         break;
      }
#else
      try
      {
         co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                      asio::deferred);
      }
      catch (const boost::system::system_error& ec)
      {
         loge("send: (range) {}", ec.what());
         break;
      }
#endif
   }

   if (eof)
   {
      logi("send: finishing request");
      co_await asio::this_coro::reset_cancellation_state();
      co_await request.async_write({}, asio::deferred);
      logi("send: done afer writing {} bytes", bytes);
   }
}

// =================================================================================================

} // namespace anyhttp
