#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <expected>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/this_coro.hpp>

#include <boost/system/detail/error_code.hpp>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/chunk.hpp>
#include <range/v3/view/iota.hpp>

using namespace std::chrono_literals;

namespace anyhttp
{
template <typename T>
using expected = std::expected<T, boost::system::error_code>;

// =================================================================================================

template <typename T>
boost::asio::awaitable<void> sleep(T duration)
{
   using namespace asio;

#if 1
#if 1
   as_tuple_t<deferred_t>::as_default_on_t<steady_timer> timer(co_await this_coro::executor);
   timer.expires_after(duration);
   auto [ec] = co_await timer.async_wait();
#else
   steady_timer timer(co_await this_coro::executor);
   timer.expires_after(duration);
   auto [ec] = co_await timer.async_wait(as_tuple(deferred));
#endif
   if (ec)
      loge("sleep: {}", ec.what());
#else
   steady_timer timer(co_await this_coro::executor);
   timer.expires_after(duration);
   try
   {
      co_await timer.async_wait(deferred);
      logi("sleep: done");
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sleep: {}", ec.what());
   }
#endif
}

boost::asio::awaitable<void> yield(size_t count = 1);
boost::asio::awaitable<void> not_found(server::Response response);
boost::asio::awaitable<void> not_found(server::Request request, server::Response response);
boost::asio::awaitable<void> dump(server::Request request, server::Response response);
boost::asio::awaitable<void> echo(server::Request request, server::Response response);
boost::asio::awaitable<void> echo_buffer(server::Request request, server::Response response);
boost::asio::awaitable<void> eat_request(server::Request request, server::Response response);

boost::asio::awaitable<void> delayed(server::Request request, server::Response response);
boost::asio::awaitable<void> detach(server::Request request, server::Response response);
boost::asio::awaitable<void> discard(server::Request request, server::Response response);

// =================================================================================================

boost::asio::awaitable<void> send(client::Request& request, size_t bytes);
boost::asio::awaitable<size_t> receive(client::Response& response);
boost::asio::awaitable<size_t> try_receive(client::Response& response,
                                           boost::system::error_code& ec);
boost::asio::awaitable<size_t> read_response(client::Request& request);
boost::asio::awaitable<expected<size_t>> try_read_response(client::Request& request);
boost::asio::awaitable<void> sendEOF(client::Request& request);

// =================================================================================================

//
// FIXME: Do we really need to restrict to "borrowed range" here? The range is kept alive in
//        the coroutine frame, so we do not need to worry about it's lifetime.
//
template <typename Range>
   requires ranges::borrowed_range<Range> && ranges::contiguous_range<Range>
boost::asio::awaitable<void> send(client::Request& request, Range range)
{
   logi("send: (continuous range)...");
   co_await request.async_write(asio::buffer(range.data(), range.size()), asio::deferred);
   logi("send: (continuous range)... done");
}

//
// For a non-contiguous range, we need to copy into a buffer first.
//
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
#if 1
      //
      // FIXME: With as_tuple<>, testcase h2spec fails, very sporadically.
      //
      //        This is also influenced by the logging level: With INFO only for the server,
      //        it happens more often than with DEBUG.
      //
      auto result = co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                                 asio::as_tuple);
      if (auto ec = std::get<0>(result))
      {
         loge("send: (range) \x1b[1;31m{}\x1b[0m after {} bytes", what(ec), bytes);
         co_return;
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
   /*
   try
   {
      co_await send(request, range);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sendAndForceEOF: {}", ec.code().message());
   }
   */
   using namespace asio;
   auto ex = co_await this_coro::executor;
   if (auto [ep] = co_await co_spawn(ex, send(request, std::move(range)), as_tuple); ep)
   {
      loge("sendAndForceEOF: {}", what(ep));
      co_await this_coro::reset_cancellation_state();
   }

   co_await sendEOF(request);
}

// -------------------------------------------------------------------------------------------------

boost::asio::awaitable<void> h2spec(server::Request request, server::Response response);

// =================================================================================================

} // namespace anyhttp
