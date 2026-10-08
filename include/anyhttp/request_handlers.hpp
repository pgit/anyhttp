#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/server.hpp"

#include <array>
#include <exception>
#include <expected>
#include <ranges>

#include <boost/asio/buffer.hpp>

#include <range/v3/view/chunk.hpp>

namespace anyhttp
{
template <typename T>
using expected = std::expected<T, error_code>;

// =================================================================================================

//
// Sleeps for \p duration, or until cancelled -- which is logged and otherwise ignored: the caller
// carries on as if the time had passed.
//
template <typename T>
Task<void> sleep(T duration)
{
   if (auto ec = co_await delay(duration))
      loge("sleep: {}", ec.message());
   else
      logi("sleep: done");
}

Task<void> yield(size_t count = 1);
Task<void> not_found(server::Response response);
Task<void> not_found(server::Request request, server::Response response);

/// Responds with 431 (Request Header Fields Too Large), see server::Config::max_header_size.
Task<void> header_fields_too_large(server::Request request, server::Response response);
Task<void> dump(server::Request request, server::Response response);
Task<void> echo(server::Request request, server::Response response);
Task<void> eat_request(server::Request request, server::Response response);

Task<void> delayed(server::Request request, server::Response response);
Task<void> detach(server::Request request, server::Response response);
Task<void> discard(server::Request request, server::Response response);

// =================================================================================================

Task<void> generate(Writer& writer, size_t bytes);
Task<std::string> read(Reader& reader);

//
// Reads and discards whatever is left of an incoming body, and returns how much that was.
//
// This is the plain shape of an ASIO read loop against the anyhttp reader interface: read until
// EOF, and let anything else -- a reset stream, a connection that went away mid-body -- come out
// as an exception.
//
Task<size_t> drain(Reader& reader);

Task<std::tuple<size_t, error_code>> try_receive(Reader& reader);
Task<size_t> try_receive(Reader& reader, error_code& ec);
Task<size_t> count_response(client::Request& request);
Task<expected<size_t>> try_read_response(client::Request& request);
Task<void> send_eof(Writer& writer);

// =================================================================================================

//
// The helpers below send a body the way the tests need it: what fails, a cancellation included,
// comes out as an exception, as the runtime throws them (anyhttp::system_error).
//

template <typename Range>
concept ByteRange =
   std::ranges::borrowed_range<Range> && (sizeof(std::ranges::range_value_t<Range>) == 1);

/// Whether \p ep holds a cancellation -- how the helpers below are often meant to end.
inline bool is_cancellation(const std::exception_ptr& ep)
{
   return code(ep) == errc::operation_canceled;
}

//
// FIXME: Do we really need to restrict to "borrowed range" here? The range is kept alive in
//        the coroutine frame, so we do not need to worry about it's lifetime.
//
template <ByteRange Range>
   requires std::ranges::contiguous_range<Range>
Task<void> send(Writer& request, Range range)
{
   logd("send: (contiguous range)...");
   if (auto [ec] = co_await request.write(asio::buffer(range.data(), range.size())); ec)
      throw_error(ec);
   logd("send: (contiguous range)... done");
}

//
// For a non-contiguous range, we need to copy into a buffer first.
//
template <ByteRange Range>
   requires(!std::ranges::contiguous_range<Range>)
Task<void> send(Writer& request, Range range)
{
   logd("send:");
   size_t bytes = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   for (auto chunk : range | ranges::views::chunk(buffer.size()))
   {
      const auto end = std::ranges::copy(chunk, buffer.data()).out;
      const auto n = static_cast<size_t>(end - buffer.data());
      if (auto [ec] = co_await request.write(asio::buffer(buffer.data(), n)); ec)
      {
         if (ec == errc::operation_canceled)
            logd("[{}] send: (range) {} after {} bytes", request.log_prefix(), ec.message(), bytes);
         else
            logw("[{}] send: (range) \x1b[1;31m{}\x1b[0m after {} bytes", request.log_prefix(),
                 ec.message(), bytes);
         throw_error(ec);
      }
      bytes += n;
   }

   logd("send: (range) sent {} bytes", bytes);
}

// -------------------------------------------------------------------------------------------------

template <ByteRange Range>
Task<void> send_and_drop(client::Request request, Range range)
{
   std::exception_ptr ep;
   try
   {
      co_await send(request, std::move(range));
   }
   catch (...)
   {
      ep = std::current_exception();
   }

   if (ep)
   {
      if (is_cancellation(ep))
         logd("[{}] send_and_drop: {}", request.log_prefix(), what(ep));
      else
         logw("[{}] send_and_drop: {}", request.log_prefix(), what(ep));
      std::rethrow_exception(ep);
   }
}

// -------------------------------------------------------------------------------------------------

/// Sends \p range and then ends the body, also when sending it failed, which is only logged.
template <ByteRange Range>
Task<void> send_and_force_eof(Writer& request, Range range)
{
   std::exception_ptr ep;
   try
   {
      co_await send(request, std::move(range));
   }
   catch (...)
   {
      ep = std::current_exception();
   }

   if (ep)
   {
      if (is_cancellation(ep))
         logd("[{}] send_and_force_eof: {}", request.log_prefix(), what(ep));
      else
         logw("[{}] send_and_force_eof: {}", request.log_prefix(), what(ep));
      co_await reset_cancellation();
   }
   std::ignore = co_await shielded(request.write_eof()); // also when sending was cancelled
}

// -------------------------------------------------------------------------------------------------

//
// Generate a body of the requested length, e.g. "/generate?length=1000000". The payload is a
// repeating 0..255 byte pattern.
//
inline Task<void> generate(server::Request request, server::Response response)
{
   namespace rv = std::ranges::views;

   const auto length = request.get_param_as<size_t>("length");
   if (!length)
   {
      std::ignore = co_await response.submit(400, {});
      std::ignore = co_await response.write_eof();
      co_return;
   }

   logd("generate: {} bytes", *length);
   if (auto [ec] = co_await response.submit(200, fields({{"Content-Length", *length}})); ec)
      co_return;
   co_await send_and_force_eof(response, rv::iota(uint8_t(0)) | rv::take(*length));
}

// -------------------------------------------------------------------------------------------------

/// Responds with "Hello, World!" and nothing else, as cheaply as the API allows.
Task<void> hello_world(server::Response response);
Task<void> h2spec(server::Request request, server::Response response);

// =================================================================================================

} // namespace anyhttp
