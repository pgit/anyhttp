
#include "anyhttp/beast/session.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/impl/write.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>

namespace beast = boost::beast;

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<boost::string_view> : ostream_formatter
{
};
template <>
struct fmt::formatter<boost::core::basic_string_view<char>> : ostream_formatter
{
};

namespace anyhttp::server
{
namespace beast_impl
{

#define mloge(x, ...) loge("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogd(x, ...) logd("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

awaitable<void> Session::do_session(std::vector<uint8_t> data)
{
   mlogd("do_session");
   m_socket.set_option(ip::tcp::no_delay(true));

   beast::tcp_stream stream(std::move(m_socket));

   // Set the timeout.
   stream.expires_after(std::chrono::seconds(30));

   bool close = false;
   beast::error_code ec;

   // This buffer is required to persist across reads
   // auto buffer = beast::flat_buffer();
   // asio::buffer_copy(buffer, asio::buffer(data));
   data.reserve(std::min(data.size(), 16 * 1024UL));
   auto buffer = boost::asio::dynamic_buffer(data);
   using namespace beast::http;
   for (;;)
   {
      request_parser<buffer_body> parser;
      auto [ec, len] = co_await async_read_header(stream, buffer, parser, as_tuple(deferred));
      mlogd("async_read_header: len={} buffer={} ec={}", len, buffer.size(), ec.message());
      if (ec)
         break;

      auto& req = parser.get();
      logd("{} {} (need_eof={})", req.method_string(), req.target(), req.need_eof());
      for (auto& header : req)
         logd("  {}: {}", header.name_string(), header.value());

      response<buffer_body> res{status::ok, parser.get().version()};
      res.set(field::server, "Beast");
      // if (req.count(field::content_type))
      //   res.set(field::content_type, req[field::content_type]);
      // res.set(field::connection, "close");
      if (req.has_content_length())
         res.content_length(parser.content_length());
      else if (req.chunked())
         res.chunked(true);
      else if (parser.is_done())
         res.content_length(0);

      res.body().data = nullptr;
      res.body().more = !parser.is_done();
      mlogd("more={} size={} buffer={}", res.body().more, res.body().size, buffer.size());

      // We need the serializer here because the serializer requires
      // a non-const file_body, and the message oriented version of
      // write only works with const messages.
      response_serializer<buffer_body> sr{res};
      co_await async_write_header(stream, sr, use_awaitable);

      // https://www.boost.org/doc/libs/1_78_0/libs/beast/doc/html/beast/using_http/parser_stream_operations/incremental_read.html
      // https://www.boost.org/doc/libs/1_78_0/libs/beast/doc/html/beast/using_http/serializer_stream_operations.html
      // https://stackoverflow.com/questions/75803164/boostbeast-http-chunked-response-buffer/75807592#75807592
      do
      {
         std::array<char, 16 * 1024> buf;
         parser.get().body().data = buf.data();
         parser.get().body().size = buf.size();

         // auto [ec, n] = co_await async_read_some(stream, buffer, parser, as_tuple(deferred));
         mlogd("async_read: ... (bytes left in buffer: {})", buffer.size());
         auto [ec, n] = co_await async_read(stream, buffer, parser, as_tuple(use_awaitable));
         // auto n = co_await async_read(stream, buffer, parser, redirect_error(use_awaitable, ec));
         mlogd("async_read: ... done, n={} (body={}) buffer={} is_done={} ({})", n,
               buf.size() - parser.get().body().size, buffer.size(), parser.is_done(),
               ec.message());
         if (n == 0)
            break;
         // std::cout << "read : ec=" << ec << " n=" << n << " / " << parser.get().body().size <<
         // std::endl;
         if (ec && ec != beast::http::error::need_buffer)
            throw std::system_error(ec, "read");
         // mlogd("[{}]", std::string_view((char*)res.body().data, res.body().size));

         res.body().data = buf.data();
         res.body().size = buf.size() - parser.get().body().size;
         res.body().more = !parser.is_done();
         if (res.body().size == 0)
            continue;

         std::tie(ec, n) = co_await async_write(stream, sr, as_tuple(deferred));
         mlogd("async_write: n={} ({})", n, ec.message());
         // std::cout << "write: ec=" << ec << " n=" << n << " / " << parser.get().body().size <<
         // std::endl;
         if (ec && ec != beast::http::error::need_buffer)
            throw std::system_error(ec, "read");
      } while (!sr.is_done());

      // Check each field promised in the "Trailer" header and output it
      for (auto const& name : token_list{req[field::trailer]})
      {
         // Find the trailer field
         auto it = req.find(name);
         if (it == req.end())
            mlogw("missing trailer: {}", name);
         else
            mlogd("  {}: {}", name, it->value());
      }
      mlogd("");
   }

   stream.close();

   // Send a TCP shutdown
   stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);

   // At this point the connection is closed gracefully

   mlogd("session done");
}

// =================================================================================================

} // namespace beast_impl
} // namespace anyhttp::server
