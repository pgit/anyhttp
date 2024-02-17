
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

using namespace std::chrono_literals;

using namespace boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace anyhttp::beast_impl
{

// =================================================================================================

template <bool IsServer>
struct Stream
{
   static constexpr bool isServer = IsServer;
   http::parser<IsServer, boost::beast::http::buffer_body> parser;
   http::response<http::buffer_body> response;
   http::serializer<!IsServer, http::buffer_body> serializer{response};
};

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

// -------------------------------------------------------------------------------------------------

// https://www.boost.org/doc/libs/1_84_0/libs/beast/doc/html/beast/using_http/parser_stream_operations/incremental_read.html
void BeastSession::async_read_some(ReadSomeHandler&& handler)
{
   mlogd("async_read_some: is_done={} size={} capacity={}", request_parser.is_done(),
         m_buffer.size(), m_buffer.capacity());

   if (request_parser.is_done())
   {
      (std::move(handler))(boost::system::error_code{}, std::vector<uint8_t>{});
      return;
   }

   request_buffer.resize(1460);
   request_parser.get().body().data = request_buffer.data();
   request_parser.get().body().size = request_buffer.size();
   boost::beast::http::async_read_some(
      m_stream, m_buffer, request_parser,
      [this, handler = std::move(handler)](boost::system::error_code ec, size_t n) mutable
      {
         auto& body = request_parser.get().body();
         size_t payload = request_buffer.size() - body.size;
         mlogd("async_read_some: n={} (body={}) ({})", n, payload, ec.message());
         if (ec == beast::http::error::need_buffer)
            ec = {}; // FIXME: maybe we should keep 'need_buffer' to avoid extra empty-buffer round
                     // trip
         if (!ec && payload == 0)
            async_read_some(std::move(handler));
         else
         {
           request_buffer.resize(payload);
            (std::move(handler))(ec, std::move(request_buffer));
         }
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

void BeastWriter::write_head(unsigned int status_code, Fields headers)
{
   session->write_head(status_code, std::move(headers));
}

void BeastWriter::async_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   session->async_write(std::move(handler), buffer);
}

void BeastWriter::async_get_response(client::Request::GetResponseHandler&& handler) {}

// -------------------------------------------------------------------------------------------------

void BeastSession::write_head(unsigned int status_code, Fields headers)
{
   response.body().data = nullptr;
   response.result(status_code);
   for (auto&& header : headers)
      response.set(header.first, header.second);
   http::write_header(m_stream, response_serializer);
   // async_write_header(m_stream, response_serializer, use_awaitable);
}

void BeastSession::async_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (buffer.size() == 0)
   {
   }

   response.body().data = const_cast<void*>(buffer.data()); // FIXME: do we really have to cast?
   response.body().size = buffer.size();
   response.body().more = buffer.size() != 0;

   http::async_write(m_stream, response_serializer,
                     [this, handler = std::move(handler)](boost::system::error_code ec,
                                                          size_t n) mutable { //
                        mlogd("async_write: n={} ({})", n, ec.message());
                        if (ec == beast::http::error::need_buffer)
                           ec = {};
                        (std::move(handler))(ec);
                     });
}

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

/**
 * This function waits for headers of an incoming, new request and passes control to a registered
 * handler. After the request has been completed, abd if the connection can be kept open, it starts
 * waiting again.
 */
awaitable<void> BeastSession::do_server_session(std::vector<uint8_t> data)
{
   mlogd("do_server_session, {} bytes in buffer", data.size());
   m_stream.socket().set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout.
   m_stream.expires_after(std::chrono::seconds(30));

   bool close = false;
   beast::error_code ec;

   // This buffer is required to persist across reads
   // auto buffer = beast::flat_buffer();
   // asio::buffer_copy(buffer, asio::buffer(data));
   // data.reserve(std::min(data.size(), 16 * 1024UL));
   // auto buffer = boost::asio::dynamic_buffer(data);
   m_data = std::move(data);
   m_data.reserve(std::min(data.size(), 16 * 1024UL));

   using namespace beast::http;
   for (;;)
   {
      auto [ec, len] =
         co_await async_read_header(m_stream, m_buffer, request_parser, as_tuple(deferred));
      mlogd("async_read_header: len={} buffer={} ec={}", len, m_buffer.size(), ec.message());
      if (ec)
         break;

      auto& req = request_parser.get();
      logd("{} {} (need_eof={})", req.method_string(), req.target(), req.need_eof());
      for (auto& header : req)
         logd("  {}: {}", header.name_string(), header.value());

      url.clear();
      url.set_path(req.target());

      this->response =
         beast::http::response<buffer_body>{status::ok, request_parser.get().version()};
      this->response.set(field::server, "anyhttp");

      server::Request request(std::make_unique<BeastReader>(*this));
      server::Response response(std::make_unique<BeastWriter>(*this));

      if (auto& handler = server().requestHandlerCoro())
         co_await handler(std::move(request), std::move(response));

      break;
#if 0
      beast::http::response<buffer_body> res{status::ok, request_parser.get().version()};
      res.set(field::server, "Beast");
      // if (req.count(field::content_type))
      //   res.set(field::content_type, req[field::content_type]);
      // res.set(field::connection, "close");
      if (req.has_content_length())
         res.content_length(request_parser.content_length());
      else if (req.chunked())
         res.chunked(true);
      else if (request_parser.is_done())
         res.content_length(0);

      res.body().data = nullptr;
      res.body().more = !request_parser.is_done();
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
         request_parser.get().body().data = buf.data();
         request_parser.get().body().size = buf.size();

         // auto [ec, n] = co_await async_read_some(stream, buffer, parser, as_tuple(deferred));
         mlogd("async_read: ... (bytes left in buffer: {})", buffer.size());
         auto [ec, n] = co_await async_read(m_stream, buffer, request_parser, as_tuple(use_awaitable));
         // auto n = co_await async_read(stream, buffer, parser, redirect_error(use_awaitable, ec));
         mlogd("async_read: ... done, n={} (body={}) buffer={} is_done={} ({})", n,
               buf.size() - request_parser.get().body().size, buffer.size(), request_parser.is_done(),
               ec.message());
         if (n == 0)
            break;
         // std::cout << "read : ec=" << ec << " n=" << n << " / " << parser.get().body().size <<
         // std::endl;
         if (ec && ec != beast::http::error::need_buffer)
            throw std::system_error(ec, "read");
         // mlogd("[{}]", std::string_view((char*)res.body().data, res.body().size));

         res.body().data = buf.data();
         res.body().size = buf.size() - request_parser.get().body().size;
         res.body().more = !request_parser.is_done();
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
#endif
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

   // response_parser<buffer_body> parser;
   // auto [ec, len] = co_await async_read_header(m_stream, buffer, parser, as_tuple(deferred));
   co_return;
}

// -------------------------------------------------------------------------------------------------

client::Request BeastSession::submit(boost::urls::url url, Fields headers)
{
   mlogd("submit: {}", url.buffer());

   return client::Request{std::make_unique<BeastWriter>(*this)};
}

// =================================================================================================

} // namespace anyhttp::beast_impl
