#include "anyhttp/request_handlers.hpp"
#include "anyhttp/client.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
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
namespace rv = std::ranges::views;

// =================================================================================================

namespace
{

struct EscapedString
{
   std::string_view str;
};

} // namespace

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

Task<void> yield(size_t count)
{
   for (size_t i = 0; i < count; ++i)
      co_await post(asio::deferred);
}

Task<void> dump(server::Request request, server::Response response)
{
   auto url = request.url();

   std::stringstream str;
   std::println(str, "method: {}", request.method());
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

   std::println(str, "headers:");
   for (const auto& field : request.fields())
      std::println(str, "  {}: {}", field.name_string(), EscapedString(field.value()));

   auto body = str.str();
   co_await response.async_submit(
      200, fields({{"Content-Length", body.size()}, {"Content-Type", "text/plain"}}));
   co_await response.async_write_eof(asio::buffer(body));
}

Task<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   co_await response.async_submit(200, {});

   std::array<uint8_t, 64_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await request.async_read_some(asio::buffer(buffer), as_tuple);
      if (ec == asio::error::eof)
         break;
      if (ec)
         throw boost::system::system_error(ec);

      co_await response.async_write(asio::buffer(buffer, n));
   }

   co_await response.async_write_eof();
}

Task<void> not_found(server::Response response)
{
   co_await response.async_submit(404, {});
   co_await response.async_write_eof();
}

Task<void> not_found(server::Request, server::Response response)
{
   co_await response.async_submit(404, {});
   co_await response.async_write_eof();
}

Task<void> header_fields_too_large(server::Request, server::Response response)
{
   co_await response.async_submit(431, {});
   co_await response.async_write_eof();
}

Task<void> eat_request(server::Request request, server::Response response)
{
   logd("eat_request: going to eat {} bytes", request.content_length().value_or(-1));

   co_await response.async_submit(200, {});
   co_await response.async_write_eof();

   try
   {
      logd("eat_request: ate {} bytes", co_await drain(request));
   }
   catch (const boost::system::system_error& e)
   {
      logi("eat_request: caught exception: {}", e.code().message());
      throw;
   }

   // co_await anyhttp::sleep(100ms);
}

Task<void> delayed(server::Request request, server::Response response)
{
   co_await sleep(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

Task<void> detach(server::Request request, server::Response response)
{
   co_await sleep(100ms);
   std::ignore = request;
   std::ignore = response;
}

Task<void> discard(server::Request request, server::Response response) { co_return; }

// =================================================================================================

Task<void> generate(Writer& writer, size_t bytes)
{
   return send_and_force_eof(writer, rv::iota(uint8_t{0}) | rv::take(bytes));
}

Task<size_t> drain(Reader& reader)
{
   size_t bytes = 0;
   std::array<uint8_t, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.async_read_some(asio::buffer(buffer), as_tuple);
      bytes += n;

      // the regular end of the body is not something to report as an error
      if (ec == asio::error::eof)
      {
         logd("drain: EOF after reading {} bytes", bytes);
         co_return bytes;
      }
      else if (ec)
      {
         logw("[{}] drain: \x1b[1;31m{}\x1b[0m after reading {} bytes, throwing",
              reader.log_prefix(), what(ec), bytes);
         throw boost::system::system_error(ec);
      }
   }
}

Task<std::string> read(Reader& reader)
{
   std::string body;
   std::array<char, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.async_read_some(asio::buffer(buffer), as_tuple);
      body += std::string_view(buffer.data(), n);
      if (ec == asio::error::eof)
      {
         logd("read: EOF after reading {} bytes", body.size());
         co_return std::move(body);
      }
      else if (ec)
      {
         // thrown on, whoever catches it reports it
         logd("[{}] read: {} after reading {} bytes", reader.log_prefix(), ec.message(),
              body.size());
         throw boost::system::system_error(ec);
      }

      logd("read: {}, total {}", n, body.size());
   }
}

Task<std::tuple<size_t, error_code>> try_receive(Reader& reader)
{
   size_t bytes = 0;
   std::array<uint8_t, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.async_read_some(asio::buffer(buffer), as_tuple);
      bytes += n;

      // the regular end of the body is not something to report as an error
      if (ec == asio::error::eof)
      {
         logd("receive: EOF after reading {} bytes", bytes);
         co_return std::make_tuple(bytes, error_code{});
      }
      else if (ec)
      {
         // handed to the caller, who decides whether that is a problem
         logi("[{}] receive: \x1b[1;31m{}\x1b[0m after reading {} bytes", reader.log_prefix(),
              ec.message(), bytes);
         co_return std::make_tuple(bytes, ec);
      }
   }
}

Task<size_t> try_receive(Reader& reader, error_code& ec)
{
   size_t bytes;
   std::tie(bytes, ec) = co_await try_receive(reader);
   co_return bytes;
}

Task<size_t> count_response(client::Request& request)
{
   auto response = co_await request.async_get_response();
   co_return co_await drain(response);
}

Task<expected<size_t>> try_read_response(client::Request& request)
{
   try
   {
      auto response = co_await request.async_get_response();
      co_return co_await drain(response);
   }
   catch (const boost::system::system_error& ex)
   {
      co_return std::unexpected(ex.code());
   }
}

Task<void> send_eof(Writer& writer) { co_await writer.async_write_eof(); }

Task<void> h2spec(server::Request request, server::Response response)
{
   co_await yield(10); // FIXME: without this, one more testcase fails
   std::array<uint8_t, 1024> buffer;
   co_await request.async_read_some(asio::buffer(buffer), as_tuple);

   constexpr auto hello = "Hello, World!\n"sv;
   co_await response.async_submit(200, fields({{"Content-Length", hello.size()}}));
   co_await response.async_write_eof(asio::buffer(hello));
   co_await drain(request);
}

// =================================================================================================

} // namespace anyhttp