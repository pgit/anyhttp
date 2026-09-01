#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <array>
#include <charconv>
#include <exception>
#include <expected>
#include <ranges>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/this_coro.hpp>

#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <range/v3/view/chunk.hpp>

using namespace std::chrono_literals;

namespace anyhttp
{
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
awaitable<void> eat_request(server::Request request, server::Response response);

awaitable<void> delayed(server::Request request, server::Response response);
awaitable<void> detach(server::Request request, server::Response response);
awaitable<void> discard(server::Request request, server::Response response);

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes);
awaitable<std::string> read(client::Response& response);

//
// Reads and discards whatever is left of an incoming body, and returns how much that was.
//
// This is the plain shape of an ASIO read loop against the anyhttp reader interface: read until
// \c asio::error::eof, and let anything else -- a reset stream, a connection that went away
// mid-body -- come out as an exception.
//
template <typename Reader>
awaitable<size_t> drain(Reader& reader)
{
   size_t bytes = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.async_read_some(asio::buffer(buffer), asio::as_tuple);
      bytes += n;
      if (ec == asio::error::eof)
         co_return bytes;
      if (ec)
         throw boost::system::system_error(ec);
   }
}

awaitable<size_t> count(client::Response& response);
awaitable<std::tuple<size_t, error_code>> try_receive(client::Response& response);
awaitable<size_t> try_receive(client::Response& response, boost::system::error_code& ec);
awaitable<size_t> read_response(client::Request& request);
awaitable<expected<size_t>> try_read_response(client::Request& request);
awaitable<void> send_eof(client::Request& request);

// =================================================================================================

template <typename Range>
concept ByteRange =
   std::ranges::borrowed_range<Range> && (sizeof(std::ranges::range_value_t<Range>) == 1);

//
// FIXME: Do we really need to restrict to "borrowed range" here? The range is kept alive in
//        the coroutine frame, so we do not need to worry about it's lifetime.
//
template <typename Writer, ByteRange Range>
   requires std::ranges::contiguous_range<Range>
awaitable<void> send(Writer& request, Range range)
{
   logd("send: (contiguous range)...");
   co_await request.async_write(asio::buffer(range.data(), range.size()));
   logd("send: (contiguous range)... done");
}

//
// For a non-contiguous range, we need to copy into a buffer first.
//
template <typename Writer, ByteRange Range>
   requires (!std::ranges::contiguous_range<Range>)
awaitable<void> send(Writer& request, Range range)
{
   logd("send:");
   size_t bytes = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   for (auto chunk : range | ranges::views::chunk(buffer.size()))
   {
      const auto end = std::ranges::copy(chunk, buffer.data()).out;
      const auto n = end - buffer.data();
      bytes += n;  // FIXME: count after async_write
#if 0
#if defined(NDEBUG)
      co_await request.async_write(asio::buffer(buffer.data(), n));
#else
      //
      // FIXME: With as_tuple<>, testcase h2spec fails, very sporadically.
      //
      //        This is also influenced by the logging level: With INFO only for the server,
      //        it happens more often than with DEBUG.
      //
      auto [ec] = co_await request.async_write(asio::buffer(buffer.data(), n),
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
         co_await request.async_write(asio::buffer(buffer.data(), n));
      }
      catch (const boost::system::system_error& ec)
      {
         loge("send: (range) \x1b[1;31m{}\x1b[0m after {} bytes", ec.code().message(), bytes);
         throw;
      }
#endif
   }

   logd("send: (range) sent {} bytes", bytes);
}

// -------------------------------------------------------------------------------------------------

template <ByteRange Range>
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

template <typename Writer, ByteRange Range>
awaitable<void> sendAndForceEOF(Writer& request, Range range)
{
   using namespace asio;
   auto ex = co_await this_coro::executor;
   if (auto [ep] = co_await co_spawn(ex, send(request, std::move(range)), as_tuple); ep)
   {
      loge("sendAndForceEOF: {}", what(ep));
      co_await asio::this_coro::reset_cancellation_state();
   }
   auto [ec] = co_await request.async_write_eof(as_tuple(deferred));
}

// -------------------------------------------------------------------------------------------------

//
// Generate a body of the requested length, e.g. "/generate?length=1000000". The payload is a
// repeating 0..255 byte pattern.
//
inline awaitable<void> generate(server::Request request, server::Response response)
{
   namespace rv = std::ranges::views;

   size_t length = 0;
   const auto param = request.url().params().get_or("length");
   auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), length);
   if (ec != std::errc{} || ptr != param.data() + param.size())
   {
      logw("generate: invalid length '{}'", param);
      co_await response.async_submit(400, {});
      co_await response.async_write_eof();
      co_return;
   }

   logd("generate: {} bytes", length);
   co_await response.async_submit(200, fields({{"Content-Length", length}}));
   co_await sendAndForceEOF(response, rv::iota(uint8_t(0)) | rv::take(length));
}

// -------------------------------------------------------------------------------------------------

awaitable<void> h2spec(server::Request request, server::Response response);

// =================================================================================================

} // namespace anyhttp
