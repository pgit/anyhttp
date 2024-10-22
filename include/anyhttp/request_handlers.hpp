#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>

#include <fmt/ostream.h>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/chunk.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

using namespace std::chrono_literals;

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

inline boost::asio::awaitable<void> yield()
{
   co_await post(co_await asio::this_coro::executor, asio::deferred);
}

boost::asio::awaitable<void> echo(server::Request request, server::Response response);
boost::asio::awaitable<void> not_found(server::Request request, server::Response response);
boost::asio::awaitable<void> eat_request(server::Request request, server::Response response);

boost::asio::awaitable<void> delayed(server::Request request, server::Response response);
boost::asio::awaitable<void> detach(server::Request request, server::Response response);
boost::asio::awaitable<void> discard(server::Request request, server::Response response);

// =================================================================================================

boost::asio::awaitable<void> send(client::Request& request, size_t bytes);
boost::asio::awaitable<size_t> receive(client::Response& response);
boost::asio::awaitable<size_t> try_receive(client::Response& response);
boost::asio::awaitable<size_t> try_receive(client::Response& response,
                                           boost::system::error_code& ec);
boost::asio::awaitable<size_t> read_response(client::Request& request);
boost::asio::awaitable<void> sendEOF(client::Request& request);

// =================================================================================================

template <typename Range>
   requires ranges::borrowed_range<Range> && ranges::contiguous_range<Range>
boost::asio::awaitable<void> send(client::Request& request, Range range)
{
   logi("send: (continous range)...");
#if 1
   try
   {
      co_await request.async_write(asio::buffer(range.data(), range.size()), asio::deferred);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("send: (contiguous range)... {}", ec.code().message());
      throw;
   }
#else
   co_await request.async_write(asio::buffer(range.data(), range.size()), asio::deferred);
#endif
   logi("send: (continous range)... done");
}

template <typename Range>
   requires ranges::borrowed_range<Range> && (!ranges::contiguous_range<Range>)
boost::asio::awaitable<void> send(client::Request& request, Range range)
{
   logi("send:");
   size_t bytes = 0;
   // std::array<uint8_t, 1460> buffer;
   std::array<uint8_t, 16 * 1024> buffer;
   for (auto chunk : range | ranges::views::chunk(buffer.size()))
   {
      auto end = std::ranges::copy(chunk, buffer.data()).out;
      bytes += end - buffer.data();
#if 0
      //
      // FIXME: With as_tuple<>, this testcase fails, sporadically.
      //
      auto result = co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                                 asio::as_tuple(asio::deferred));
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
         loge("send: (range) {}, sent {} bytes", ec.code().message(), bytes);
         break;
      }
#endif
   }
   logi("send: (range) sent {} bytes", bytes);
}

// -------------------------------------------------------------------------------------------------

template <typename Range>
boost::asio::awaitable<void> sendAndForceEOF(client::Request& request, Range range)
{
   try
   {
      co_await send(request, range);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sendAndForceEOF: {}", ec.code().message());
   }

   co_await asio::this_coro::reset_cancellation_state();
   co_await sendEOF(request);
}

// =================================================================================================

} // namespace anyhttp
