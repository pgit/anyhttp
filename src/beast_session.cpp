
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
#include "anyhttp/server.hpp"

using namespace std::chrono_literals;

using namespace boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace anyhttp::beast_impl
{

// =================================================================================================

class BeastSession;

template <typename Parent, typename Stream, typename Buffer, typename Parser>
class BeastReader : public Parent
{
public:
   // explicit BeastReader(BeastSession& session) {}
   inline BeastReader(BeastSession& session_, Stream& stream_, Buffer& buffer_)
      : session(&session_), stream(stream_), buffer(buffer_)
   {
   }

   ~BeastReader() override {}
   void detach() override { session = nullptr; }

   boost::url_view url() const override { return session->url; }
   std::optional<size_t> content_length() const noexcept override
   {
      if (parser.content_length())
         return parser.content_length().value();
      else
         return std::nullopt;
   }

   void async_read_some(server::Request::ReadSomeHandler&& handler) override
   {
      mlogd("async_read_some: is_done={} size={} capacity={}", parser.is_done(), buffer.size(),
            buffer.capacity());

      if (parser.is_done())
      {
         (std::move(handler))(boost::system::error_code{}, std::vector<uint8_t>{});
         return;
      }

      std::vector<uint8_t> body_buffer;
      body_buffer.resize(1460);
      parser.get().body().data = body_buffer.data();
      parser.get().body().size = body_buffer.size();
      boost::beast::http::async_read_some(
         stream, buffer, parser,
         [this, body_buffer = std::move(body_buffer),
          handler = std::move(handler)](boost::system::error_code ec, size_t n) mutable
         {
            auto& body = parser.get().body();
            size_t payload = body_buffer.size() - body.size;
            mlogd("async_read_some: n={} (body={}) ({}) is_done={} size={} capacity={}", n, payload,
                  ec.message(), parser.is_done(), buffer.size(), buffer.capacity());
            if (ec == beast::http::error::need_buffer)
               ec = {}; // FIXME: maybe we should keep 'need_buffer' to avoid extra empty round trip

            if (!ec && payload == 0)
               async_read_some(std::move(handler));
            else
            {
               body_buffer.resize(payload);
               (std::move(handler))(ec, std::move(body_buffer));
            }
         });
   }

   const asio::any_io_executor& executor() const override { return session->executor(); }
   inline auto logPrefix() const { return session->logPrefix(); }

   BeastSession* session;
   Stream& stream;
   Buffer& buffer;
   Parser parser;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream, typename Message, typename Serializer>
class BeastWriter : public server::Response::Impl, public client::Request::Impl
{
public:
   inline BeastWriter(BeastSession& session_, Stream& stream_) : session(&session_), stream(stream_)
   {
   }

   ~BeastWriter() override {}
   void detach() override { session = nullptr; }

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         message.content_length(*content_length);
      else
         message.content_length(boost::none);
   }

   void write_head(unsigned int status_code, Fields headers) override
   {
      mlogd("write_head:");

      message.body().data = nullptr;
      if constexpr (!Message::is_request::value)
         message.result(status_code);
      else
         message.method(http::verb::get);

      for (auto&& header : headers)
         message.set(header.first, header.second);

      if (!message.has_content_length())
         message.chunked(true);

      http::write_header(stream, serializer);
      // async_write_header(m_stream, response_serializer, use_awaitable);
   }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override
   {
      if (buffer.size() == 0)
         mlogd("async_write: write EOF");
      else
         mlogd("async_write: {} bytes", buffer.size());

      message.body().data = const_cast<void*>(buffer.data()); // FIXME: do we really have to cast?
      message.body().size = buffer.size();
      message.body().more = buffer.size() != 0;

      http::async_write(stream, serializer,
                        [this, handler = std::move(handler)](boost::system::error_code ec,
                                                             size_t n) mutable { //
                           mlogd("async_write: n={} ({})", n, ec.message());
                           if (ec == beast::http::error::need_buffer)
                              ec = {};
                           (std::move(handler))(ec);
                        });
   }

   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      auto& buffer = session->m_buffer;
      auto& stream = session->m_stream;

      auto reader =
         std::make_unique<BeastReader<client::Response::Impl, decltype(stream), decltype(buffer),
                                      http::response_parser<http::buffer_body>>>(*session, stream,
                                                                                 buffer);
      auto& parser = reader->parser;

      logd("");
      mlogd("waiting for response (size={} capacity={})", buffer.size(), buffer.capacity());
      async_read_header(stream, buffer, parser,
                        [reader = std::move(reader), handler = std::move(handler),
                         this](boost::system::error_code ec, size_t len) mutable
                        {
                           if (!ec)
                              mlogd("async_read_header: len={}", ec.message(), len);
                           else
                              mlogw("async_read_header: {} len={}", ec.message(), len);
                           std::move(handler)(ec, client::Response(std::move(reader)));
                        });
   }

   const asio::any_io_executor& executor() const override { return session->executor(); }
   inline auto logPrefix() const { return session->logPrefix(); }

   BeastSession* session;
   Stream& stream;
   Message message;
   Serializer serializer{message};

   client::Request::GetResponseHandler responseHandler;
};

// =================================================================================================

BeastSession::BeastSession(std::string_view log, asio::any_io_executor executor,
                           asio::ip::tcp::socket&& socket)
   : m_executor(std::move(executor)), m_stream(std::move(socket)),
     m_logPrefix(fmt::format("{} {}", normalize(m_stream.socket().remote_endpoint()), log))
{
   mlogd("session created");
   m_send_buffer.resize(64 * 1024);
}

BeastSession::BeastSession(server::Server::Impl& parent, any_io_executor executor,
                           ip::tcp::socket&& socket)
   : BeastSession("\x1b[1;31mserver\x1b[0m", std::move(executor), std::move(socket))
{
   m_server = &parent;
}

BeastSession::BeastSession(client::Client::Impl& parent, any_io_executor executor,
                           ip::tcp::socket&& socket)
   : BeastSession("\x1b[1;32mclient\x1b[0m", std::move(executor), std::move(socket))
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

   // Set the timeout. TODO: don't rely on beast timeouts
   m_stream.expires_after(std::chrono::seconds(30));

   bool close = false;
   beast::error_code ec;

   // This buffer is required to persist across reads
   // auto buffer = beast::flat_buffer();
   // asio::buffer_copy(buffer, asio::buffer(data));
   // data.reserve(std::min(data.size(), 16 * 1024UL));
   // auto buffer = boost::asio::dynamic_buffer(data);
   m_data = std::move(data);
   // m_data.reserve(std::max(data.size(), 16 * 1024UL));

   size_t requestCounter = 0;
   for (;;)
   {
      auto reader =
         std::make_unique<BeastReader<server::Request::Impl, decltype(m_stream), decltype(m_buffer),
                                      http::request_parser<http::buffer_body>>>(*this, m_stream,
                                                                                m_buffer);
      auto& parser = reader->parser;

      logd("");
      mlogd("waiting for request (size={} capacity={})", m_buffer.size(), m_buffer.capacity());
      auto [ec, len] = co_await async_read_header(m_stream, m_buffer, parser, as_tuple(deferred));
      if (!ec)
         mlogd("async_read_header: len={} size={} capacity={} ec={}", len, m_buffer.size(),
               m_buffer.capacity(), ec.message());
      else
         mlogw("async_read_header: len={} size={} capacity={} ec={}", len, m_buffer.size(),
               m_buffer.capacity(), ec.message());
      if (ec)
         break;
      requestCounter++;

      auto& request = parser.get();
      mlogd("{} {} (need_eof={})", request.method_string(), request.target(), request.need_eof());
      for (auto& header : request)
         mlogd("  {}: {}", header.name_string(), header.value());

      url.clear();
      url.set_path(request.target());

      server::Request request_wrapper(std::move(reader));

      //
      // Prepare response.
      //
      auto writer =
         std::make_unique<BeastWriter<decltype(m_stream), http::response<http::buffer_body>,
                                      http::response_serializer<http::buffer_body>>>(*this,
                                                                                     m_stream);
      auto& response = writer->message;
      response.set(http::field::server, "anyhttp");

      server::Response response_wrapper(std::move(writer));

      //
      // Call user-provided request handler.
      //
      if (auto& handler = server().requestHandlerCoro())
         co_await handler(std::move(request_wrapper), std::move(response_wrapper));

      //
      // FIXME: We need to wait for request and response
      //
      mlogd("request handler finished (size={} capacity={})", m_buffer.size(), m_buffer.capacity());

      if (request.need_eof())
      {
         mlogd("request needs EOF, closing connection");
         break;
      }

      if (response.need_eof())
      {
         mlogd("response needs EOF, closing connection");
         break;
      }
   }

   mlogi("closing stream, served {} requests", requestCounter);

   m_stream.close();
   std::ignore = m_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);

   mlogd("session done");
}

// -------------------------------------------------------------------------------------------------

awaitable<void> BeastSession::do_client_session(std::vector<uint8_t> data)
{
   mlogd("do_client_session");
   m_stream.socket().set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout.
   m_stream.expires_after(std::chrono::seconds(30));

   // This buffer is required to persist across reads
   // auto buffer = beast::flat_buffer();
   // asio::buffer_copy(buffer, asio::buffer(data));
   data.reserve(std::min(data.size(), 16 * 1024UL));
   auto buffer = boost::asio::dynamic_buffer(data);
   using namespace beast::http;

   co_return;

   //
   // TEST: wait
   //
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(2s);
   co_await timer.async_wait(deferred);

   // auto [ec, len] = co_await async_read_header(m_stream, buffer, parser, as_tuple(deferred));
   co_return;
}

// -------------------------------------------------------------------------------------------------

client::Request BeastSession::submit(boost::urls::url url, Fields headers)
{
   mlogd("submit: {}", url.buffer());

   auto writer =
      std::make_unique<BeastWriter<decltype(m_stream), http::request<http::buffer_body>,
                                   http::request_serializer<http::buffer_body>>>(*this, m_stream);
   auto& request = writer->message;

   request.base().target("/echo");
   request.method(http::verb::post);
   request.set(http::field::user_agent, "anyhttp");
   for (auto&& header : headers)
      request.set(header.first, header.second);
   if (!request.has_content_length())
      request.chunked(true);

   return client::Request(std::move(writer));
}

// =================================================================================================

} // namespace anyhttp::beast_impl
