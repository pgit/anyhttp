#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <exception>
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
using error_code = boost::system::error_code;
template <typename T>
using expected = std::expected<T, boost::system::error_code>;

// =================================================================================================

template <typename T>
awaitable<void> sleep(T duration)
{
   using namespace asio;

#if 0
#if 1
   as_tuple_t<deferred_t>::as_default_on_t<steady_timer> timer(co_await this_coro::executor);
   timer.expires_after(duration);
   auto [ec] = co_await timer.async_wait();
#else
   steady_timer timer(co_await this_coro::executor);
   timer.expires_after(duration);
   auto [ec] = co_await timer.async_wait(as_tuple);
#endif
   if (ec)
      loge("sleep: {}", ec.what());
#else
   steady_timer timer(co_await this_coro::executor);
   timer.expires_after(duration);
   try
   {
      co_await timer.async_wait();
      logi("sleep: done");
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sleep: {}", ec.what());
   }
#endif
}

awaitable<void> yield(size_t count = 1);
awaitable<void> not_found(server::Response response);
awaitable<void> not_found(server::Request request, server::Response response);
awaitable<void> dump(server::Request request, server::Response response);
awaitable<void> echo(server::Request request, server::Response response);
awaitable<void> echo_buffer(server::Request request, server::Response response);
awaitable<void> eat_request(server::Request request, server::Response response);

awaitable<void> delayed(server::Request request, server::Response response);
awaitable<void> detach(server::Request request, server::Response response);
awaitable<void> discard(server::Request request, server::Response response);

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes);
awaitable<size_t> receive(client::Response& response);
awaitable<std::tuple<size_t, error_code>> try_receive(client::Response& response);
awaitable<size_t> try_receive(client::Response& response, boost::system::error_code& ec);
awaitable<size_t> read_response(client::Request& request);
awaitable<expected<size_t>> try_read_response(client::Request& request);
awaitable<void> sendEOF(client::Request& request);

// =================================================================================================

//
// FIXME: Do we really need to restrict to "borrowed range" here? The range is kept alive in
//        the coroutine frame, so we do not need to worry about it's lifetime.
//
template <typename Range>
   requires ranges::borrowed_range<Range> && ranges::contiguous_range<Range>
awaitable<void> send(client::Request& request, Range range)
{
   logi("send: (contiguous range)...");
   co_await request.async_write(asio::buffer(range.data(), range.size()));
   logi("send: (contiguous range)... done");
}

//
// For a non-contiguous range, we need to copy into a buffer first.
//
template <typename Range>
   requires ranges::borrowed_range<Range> && (!ranges::contiguous_range<Range>)
awaitable<void> send(client::Request& request, Range range)
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
#if defined(NDEBUG)
      co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()));
#else
      //
      // FIXME: With as_tuple<>, testcase h2spec fails, very sporadically.
      //
      //        This is also influenced by the logging level: With INFO only for the server,
      //        it happens more often than with DEBUG.
      //
      auto [ec] = co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                               asio::as_tuple);
      if (ec)
      {
         loge("send: (range) \x1b[1;31m{}\x1b[0m after {} bytes", what(ec), bytes);
         throw boost::system::system_error(ec);
      }
#endif
#else
      try
      {
         co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()));
      }
      catch (const boost::system::system_error& ec)
      {
         loge("send: (range) \x1b[1;31m{}\x1b[0m after {} bytes", ec.code().message(), bytes);
         throw;
      }
#endif
   }
   logi("send: (range) sent {} bytes", bytes);
}

// -------------------------------------------------------------------------------------------------

template <typename Range>
   requires ranges::borrowed_range<Range>
awaitable<void> sendAndDrop(client::Request request, Range range)
{
#if 0
   try
   {
      co_return co_await send(request, std::move(range));
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sendAndDrop: (range) {}", ec.code().message());
      throw;
   }
#else
   using namespace asio;
   auto ex = co_await this_coro::executor;
   if (auto [ep] = co_await co_spawn(ex, send(request, std::move(range)), as_tuple); ep)
   {
      loge("sendAndDrop: {}", what(ep));
      std::rethrow_exception(ep);
   }
#endif
}

// -------------------------------------------------------------------------------------------------

template <typename Range>
awaitable<void> sendAndForceEOF(client::Request& request, Range range)
{
#if 0
   try
   {
      co_await send(request, range);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sendAndForceEOF: {}", ec.code().message());
   }
   co_await asio::this_coro::reset_cancellation_state();
#else
   using namespace asio;
   auto ex = co_await this_coro::executor;
   if (auto [ep] = co_await co_spawn(ex, send(request, std::move(range)), as_tuple); ep)
   {
      loge("sendAndForceEOF: {}", what(ep));
      co_await asio::this_coro::reset_cancellation_state();
   }
#endif
   co_await sendEOF(request);
}

// -------------------------------------------------------------------------------------------------

awaitable<void> h2spec(server::Request request, server::Response response);

// =================================================================================================

} // namespace anyhttp
