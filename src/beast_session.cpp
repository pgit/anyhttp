
#include "anyhttp/beast_session.hpp"

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

using namespace boost::asio;
namespace beast = boost::beast;

namespace anyhttp::beast_impl
{

// =================================================================================================

BeastReader::BeastReader(BeastSession& session_)
   : server::Request::Impl(), client::Response::Impl(), session(&session_)
{
}

BeastReader::~BeastReader() {}

void BeastReader::detach() { session = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& BeastReader::executor() const
{
   assert(session);
   return session->executor();
}

boost::url_view BeastReader::url() const
{
   assert(session);
   return {session->url};
}

void BeastReader::async_read_some(ReadSomeHandler&& handler)
{
   assert(session);
   session->async_read_some(std::move(handler));
}

void BeastSession::async_read_some(ReadSomeHandler&& handler)
{
   std::vector<uint8_t> data;
   auto buffer = boost::asio::dynamic_buffer(data);

   boost::beast::http::async_read_some(m_stream, buffer, request_parser,
                                       [data = std::move(data), handler = std::move(handler)](
                                          const boost::system::error_code& ec, size_t n) mutable
                                       {
                                          data.resize(n);
                                          (std::move(handler))(ec, data);
                                       });
}

// =================================================================================================

BeastWriter::BeastWriter(BeastSession& session_)
   : server::Response::Impl(), client::Request::Impl(), session(&session_)
{
}

BeastWriter::~BeastWriter() {}

void BeastWriter::detach() { session = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& BeastWriter::executor() const
{
   assert(session);
   return session->executor();
}

void BeastWriter::write_head(unsigned int status_code, Fields headers) {}

void BeastWriter::async_write(WriteHandler&& handler, asio::const_buffer buffer) {}

void BeastWriter::async_get_response(client::Request::GetResponseHandler&& handler) {}

// =================================================================================================

BeastSession::BeastSession(asio::any_io_executor executor, asio::ip::tcp::socket&& socket)
   : m_executor(std::move(executor)), m_stream(std::move(socket)),
     m_logPrefix(fmt::format("{}", normalize(m_stream.socket().remote_endpoint())))
{
   mlogd("session created");
   m_send_buffer.resize(64 * 1024);
}

BeastSession::BeastSession(server::Server::Impl& parent, any_io_executor executor,
                           ip::tcp::socket&& socket)
   : BeastSession(std::move(executor), std::move(socket))
{
   m_server = &parent;
}

BeastSession::BeastSession(client::Client::Impl& parent, any_io_executor executor,
                           ip::tcp::socket&& socket)
   : BeastSession(std::move(executor), std::move(socket))
{
   m_client = &parent;
   // create_client_session();
}

BeastSession::~BeastSession() { mlogd("session destroyed"); }

// =================================================================================================

awaitable<void> BeastSession::do_server_session(std::vector<uint8_t> data)
{
   mlogd("do_server_session");
   m_stream.socket().set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout.
   m_stream.expires_after(std::chrono::seconds(30));

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
      boost::beast::http::request_parser<buffer_body> parser;
      auto [ec, len] = co_await async_read_header(m_stream, buffer, parser, as_tuple(deferred));
      mlogd("async_read_header: len={} buffer={} ec={}", len, buffer.size(), ec.message());
      if (ec)
         break;

      server::Request request(std::make_unique<BeastReader>(*this));
      server::Response response(std::make_unique<BeastWriter>(*this));

      auto& req = parser.get();
      logd("{} {} (need_eof={})", req.method_string(), req.target(), req.need_eof());
      for (auto& header : req)
         logd("  {}: {}", header.name_string(), header.value());

      url.clear();
      url.set_path(req.target());

      beast::http::response<buffer_body> res{status::ok, parser.get().version()};
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
      co_await async_write_header(m_stream, sr, use_awaitable);

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
         auto [ec, n] = co_await async_read(m_stream, buffer, parser, as_tuple(use_awaitable));
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

         std::tie(ec, n) = co_await async_write(m_stream, sr, as_tuple(deferred));
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

   m_stream.close();

   // Send a TCP shutdown
   m_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);

   // At this point the connection is closed gracefully

   mlogd("session done");
}

// -------------------------------------------------------------------------------------------------

awaitable<void> BeastSession::do_client_session(std::vector<uint8_t> data)
{
   mlogd("do_server_session");
   m_stream.socket().set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout.
   m_stream.expires_after(std::chrono::seconds(30));

   // This buffer is required to persist across reads
   // auto buffer = beast::flat_buffer();
   // asio::buffer_copy(buffer, asio::buffer(data));
   data.reserve(std::min(data.size(), 16 * 1024UL));
   auto buffer = boost::asio::dynamic_buffer(data);
   using namespace beast::http;

   response_parser<buffer_body> parser;
   auto [ec, len] = co_await async_read_header(m_stream, buffer, parser, as_tuple(deferred));
}

// -------------------------------------------------------------------------------------------------

client::Request BeastSession::submit(boost::urls::url url, Fields headers)
{
   mlogd("submit: {}", url.buffer());

   return client::Request{std::make_unique<BeastWriter>(*this)};
}

// =================================================================================================

} // namespace anyhttp::beast_impl
