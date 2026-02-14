#include "anyhttp/request_handlers.hpp"
#include "anyhttp/client.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/server.hpp"

#include <boost/asio/deferred.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/algorithm/string/replace.hpp>
#include <boost/system/detail/error_code.hpp>

#include <ranges>
#include <tuple>

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;
using boost::system::error_code;
namespace rv = std::ranges::views;

// =================================================================================================

struct EscapedString
{
   std::string_view str;
};

template <>
struct std::formatter<EscapedString> : std::formatter<std::string>
{
   template <typename FormatContext>
   auto format(const EscapedString& esc, FormatContext& ctx) const
   {
      std::string result;
      for (unsigned char ch : esc.str)
         if (ch < 32 || ch >= 127)
            std::format_to(std::back_inserter(result), "\x1b[33m%\x1b[34;1m{:02X}\x1b[0m", ch);
         else
            result.push_back(ch);
      return std::formatter<std::string>::format(result, ctx);
   }
};

// =================================================================================================

namespace anyhttp
{

awaitable<void> yield(size_t count)
{
   auto executor = co_await asio::this_coro::executor;
   for (size_t i = 0; i < count; ++i)
      co_await post(executor, asio::deferred);
}

awaitable<void> dump(server::Request request, server::Response response)
{
   auto url = request.url();

   std::stringstream str;
   std::println(str, "RAW URL: {}", url.buffer());
   std::println(str, "authority: {} ({})", url.authority(), url.encoded_authority());
   std::println(str, "path: {} ({})", url.path(), url.encoded_path());
   for (auto segment : url.segments())
      std::println(str, "  {}", EscapedString(segment));

   std::println(str, "query: {}", EscapedString(url.query()));
   std::println(str, "query: {} (encoded)", url.encoded_query());
   for (auto [key, value, _] : url.params())
      std::println(str, "  {}={} ({})", key, EscapedString(value), _);
   std::println(str, "fragment: {} ({})", url.fragment(), url.encoded_fragment());

   Fields fields;
   fields.set("Content-Length", std::format("{}", str.str().size()));
   fields.set("Content-Type", "text/plain");
   co_await response.async_submit(200, fields);
   co_await response.async_write(asio::buffer(str.str()));
   co_await response.async_write({}, deferred);
}

awaitable<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   co_await response.async_submit(200, {});

   std::array<uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      size_t n = co_await request.async_read_some(asio::buffer(buffer));
      co_await response.async_write(asio::buffer(buffer, n));
      if (n == 0)
         break;
   }
}

awaitable<void> not_found(server::Response response)
{
   co_await response.async_submit(404, {});
   co_await response.async_write({});
}

awaitable<void> not_found(server::Request, server::Response response)
{
   co_await response.async_submit(404, {});
   co_await response.async_write({});
}

awaitable<void> eat_request(server::Request request, server::Response response)
{
   logi("eat_request: going to eat {} bytes", request.content_length().value_or(-1));

   co_await response.async_submit(200, {});
   co_await response.async_write({});

   size_t bytes = 0;
   try
   {
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         size_t n = co_await request.async_read_some(asio::buffer(buffer));
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
   co_await sleep(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

awaitable<void> detach(server::Request request, server::Response response)
{
   co_await sleep(100ms);
   std::ignore = request;
   std::ignore = response;
}

awaitable<void> discard(server::Request request, server::Response response) { co_return; }

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes)
{
   return sendAndForceEOF(request, rv::iota(uint8_t{0}) | rv::take(bytes));
}

awaitable<std::string> read(client::Response& response)
{
   std::string body;
   std::array<char, 1024> buffer;
   for (;;)
   {
      size_t n = co_await response.async_read_some(asio::buffer(buffer));
      if (n == 0)
         break;

      body += std::string_view(buffer.data(), n);
      logd("read: {}, total {}", n, body.size());
   }

   logi("read: EOF after reading {} bytes", body.size());
   co_return body;
}

awaitable<size_t> count(client::Response& response)
{
   size_t bytes = 0;
   std::array<uint8_t, 1024> buffer;
   for (;;)
   {
      size_t n = co_await response.async_read_some(asio::buffer(buffer));
      if (n == 0)
         break;

      bytes += n;
      logd("count: {}, total {}", n, bytes);
   }

   logi("count: EOF after reading {} bytes", bytes);
   co_return bytes;
}

awaitable<std::tuple<size_t, error_code>> try_receive(client::Response& response)
{
   size_t bytes = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), as_tuple);
      // co_await yield();
      bytes += n;
      if (ec || n == 0)
         co_return std::make_tuple(bytes, ec);
   }
}

awaitable<size_t> try_receive(client::Response& response, error_code& ec)
{
#if 0
   size_t bytes;
   std::tie(bytes, ec) = co_await try_receive(response);
#else
   ec = {};
   size_t bytes = 0, count = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   try
   {
      for (;;)
      {
         size_t n = co_await response.async_read_some(asio::buffer(buffer));
         if (n == 0)
            break;

         // do NOT 'respawn' read handler in first round, see NGHttp2Stream::call_handler_loop()
         // if (count++ == 0)
         //    co_await yield();
         // co_await yield();

         bytes += n;
         logd("receive: {}, total {}", n, bytes);
      }
   }
   catch (const boost::system::system_error& ex)
   {
      ec = ex.code();
      loge("receive: \x1b[1;31n{}\x1b[0m after reading {} bytes", ex.code().message(), bytes);
      co_return bytes;
   }

   // co_await sleep(100ms);

   logi("receive: EOF after reading {} bytes", bytes);
   co_return bytes;
#endif
}

awaitable<size_t> read_response(client::Request& request)
{
   auto response = co_await request.async_get_response();
   co_return co_await count(response);
}

awaitable<expected<size_t>> try_read_response(client::Request& request)
{
   try
   {
      auto response = co_await request.async_get_response();
      co_return co_await count(response);
   }
   catch (const boost::system::system_error& ex)
   {
      co_return std::unexpected(ex.code());
   }
}

awaitable<void> send_eof(client::Request& request)
{
   co_await request.async_write({});
   // logi("send: finishing request...");
   // auto [ec] = co_await request.async_write({}, as_tuple(deferred));
   // logi("send: finishing request... done ({})", ec.message());
}

awaitable<void> h2spec(server::Request request, server::Response response)
{
   co_await yield(10);
   std::array<uint8_t, 1024> buffer;
   size_t n = co_await request.async_read_some(asio::buffer(buffer));
   co_await response.async_submit(200, {});
   co_await response.async_write(asio::buffer("Hello, World!\n"sv));
   co_await response.async_write({});
   while (co_await request.async_read_some(asio::buffer(buffer)) > 0)
      ;
}

// =================================================================================================

} // namespace anyhttp