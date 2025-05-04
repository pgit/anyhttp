#include "anyhttp/request_handlers.hpp"
#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>
#include <sstream>

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;
namespace rv = ranges::views;

struct EscapedString
{
   std::string_view str;
};

template <>
struct fmt::formatter<EscapedString> : fmt::formatter<std::string>
{
   auto format(const EscapedString& esc, fmt::format_context& ctx) const
      -> fmt::format_context::iterator
   {
      std::string result;
      for (unsigned char ch : esc.str)
         if (ch < 32 || ch >= 127)
            fmt::format_to(std::back_inserter(result), "\x1b[33m%\x1b[34;1m{:02X}\x1b[0m", ch);
         else
            result.push_back(ch);
      return fmt::formatter<std::string>::format(result, ctx);
   }
};

namespace anyhttp
{

// =================================================================================================

boost::asio::awaitable<void> yield(size_t count)
{
   auto ex = co_await asio::this_coro::executor;
   for (size_t i = 0; i < count; ++i)
      co_await post(ex, asio::deferred);
}

awaitable<void> dump(server::Request request, server::Response response)
{
   auto url = request.url();

   std::stringstream str;
   fmt::println(str, "RAW URL: {}", url.buffer());
   fmt::println(str, "authority: {} ({})", url.authority(), url.encoded_authority());
   fmt::println(str, "path: {} ({})", url.path(), url.encoded_path());
   for (auto segment : url.segments())
      fmt::println(str, "  {}", EscapedString(segment));

   fmt::println(str, "query: {}", EscapedString(url.query()));
   fmt::println(str, "query: {} (encoded)", url.encoded_query());
   for (auto [key, value, _] : url.params())
      fmt::println(str, "  {}={} ({})", key, EscapedString(value), _);
   fmt::println(str, "fragment: {} ({})", url.fragment(), url.encoded_fragment());

   auto buf = str.str();
   co_await response.async_submit(200,
                                  {{"Content-Length", fmt::format("{}", buf.size())}, //
                                   {"Content-Type", "text/plain"}},
                                  deferred);
   co_await response.async_write(asio::buffer(str.str()), deferred);
   co_await response.async_write({}, deferred);
}

awaitable<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   co_await response.async_submit(200, {}, deferred);

   std::array<uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      size_t n = co_await request.async_read_some(asio::buffer(buffer), deferred);
      co_await response.async_write(asio::buffer(buffer, n), deferred);
      if (n == 0)
         break;
   }
}

awaitable<void> not_found(server::Response response)
{
   co_await response.async_submit(404, {}, deferred);
   co_await response.async_write({}, deferred);
}

awaitable<void> not_found(server::Request, server::Response response)
{
   co_await response.async_submit(404, {}, deferred);
   co_await response.async_write({}, deferred);
}

awaitable<void> eat_request(server::Request request, server::Response response)
{
   logi("eat_request: going to eat {} bytes", request.content_length().value_or(-1));

   co_await response.async_submit(200, {}, deferred);
   co_await response.async_write({}, deferred);

   size_t bytes = 0;
   try
   {
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         size_t n = co_await request.async_read_some(asio::buffer(buffer), deferred);
         if (n == 0)
            break;

         logd("eat_request: ate {} bytes", n);
         bytes += n;
      }
      logi("eat_request: ate {} bytes", bytes);
   }
   catch (const boost::system::system_error& e)
   {
      logi("eat_request: ate {} bytes, then caught exception: {}", bytes, e.code().message());
      throw;
   }

   // co_await anyhttp::sleep(100ms);
}

awaitable<void> delayed(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_after(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

awaitable<void> detach(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_after(100ms);
   // co_await eat_request(std::move(request), std::move(response));
}

awaitable<void> discard(server::Request request, server::Response response) { co_return; }

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes)
{
   return sendAndForceEOF(request, rv::iota(0) | rv::take(bytes));
}

awaitable<size_t> receive(client::Response& response)
{

   size_t bytes = 0;
   std::array<uint8_t, 1024> buffer;
   for (;;)
   {
      size_t n = co_await response.async_read_some(asio::buffer(buffer), deferred);
      if (n == 0)
         break;

      bytes += n;
      logd("receive: {}, total {}", n, bytes);
   }

   logi("receive: EOF after reading {} bytes", bytes);
   co_return bytes;
}

awaitable<size_t> try_receive(client::Response& response, boost::system::error_code& ec)
{
   ec = {};
   size_t bytes = 0, count = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   try
   {
      for (;;)
      {
         size_t n = co_await response.async_read_some(asio::buffer(buffer), deferred);
         if (n == 0)
            break;

         // do NOT 'respawn' read handler in first round, see NGHttp2Stream::call_handler_loop()
         if (count == 0)
            co_await yield();

         bytes += n;
         logd("receive: {}, total {}", n, bytes);
      }
   }
   catch (const boost::system::system_error& ex)
   {
      ec = ex.code();
      loge("receive: {} after reading {} bytes", ex.code().message(), bytes);
      co_return bytes;
   }

   // co_await sleep(100ms);

   logi("receive: EOF after reading {} bytes", bytes);
   co_return bytes;
}

awaitable<size_t> read_response(client::Request& request)
{
   auto response = co_await request.async_get_response(asio::deferred);
   co_return co_await receive(response);
}

awaitable<expected<size_t>> try_read_response(client::Request& request)
{
   try
   {
      auto response = co_await request.async_get_response(asio::deferred);
      co_return co_await receive(response);
   }
   catch (const boost::system::system_error& ex)
   {
      co_return std::unexpected(ex.code());
   }
}

awaitable<void> sendEOF(client::Request& request)
{
   logi("send: finishing request...");
   auto [ec] = co_await request.async_write({}, as_tuple(deferred));
   logi("send: finishing request... done ({})", ec.what());
}

awaitable<void> h2spec(server::Request request, server::Response response)
{
   co_await yield(10);
   std::array<uint8_t, 1024> buffer;
   size_t n = co_await request.async_read_some(asio::buffer(buffer), deferred);
   co_await yield(); // ok
   co_await response.async_submit(200, {}, deferred);
   co_await yield(); // ok
   const char* literal = "Hello, World!\n";
   co_await response.async_write(asio::const_buffer(literal, std::strlen(literal)), deferred);
   co_await yield(); // ok
   co_await response.async_write({}, deferred);
   co_await yield(); // ok
   while (co_await request.async_read_some(asio::buffer(buffer), deferred) > 0)
      ;
}

// =================================================================================================

} // namespace anyhttp