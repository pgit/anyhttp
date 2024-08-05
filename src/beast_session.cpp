
#include "anyhttp/beast_session.hpp"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/impl/write.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/version.hpp>
#include <boost/smart_ptr/make_shared_array.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/generic_category.hpp>
#include "anyhttp/server.hpp"

using namespace std::chrono_literals;

using namespace boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace anyhttp::beast_impl
{

// =================================================================================================

using namespace asio;
using namespace boost::beast;
using socket = asio::ip::tcp::socket;

inline auto& get_socket(socket& socket) { return socket; }
inline auto& get_socket(tcp_stream& stream) { return stream.socket(); }
inline auto& get_socket(ssl::stream<socket>& stream) { return stream.lowest_layer(); }

// =================================================================================================

template <typename Parent, typename Stream, typename Buffer, typename Parser>
class BeastReader : public Parent
{
public:
   inline BeastReader(BeastSession<Stream>& session_, Stream& stream_, Buffer& buffer_)
      : session(&session_), stream(stream_), buffer(buffer_)
   {
      parser.body_limit(std::numeric_limits<uint64_t>::max());
   }

   ~BeastReader() override {}
   void detach() override { session = nullptr; }

   boost::url_view url() const override { return m_url; }
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
         std::move(handler)(boost::system::error_code{}, std::vector<uint8_t>{});
         return;
      }

      std::vector<uint8_t> body_buffer;
      // body_buffer.resize(1460);
      body_buffer.resize(64 * 1024);
      parser.get().body().data = body_buffer.data();
      parser.get().body().size = body_buffer.size();
      auto cb = [this, body_buffer = std::move(body_buffer),
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
      };

      boost::beast::http::async_read_some(stream, buffer, parser, std::move(cb));
   }

   const asio::any_io_executor& executor() const override { return session->executor(); }
   inline auto logPrefix() const { return session->logPrefix(); }

   BeastSession<Stream>* session;
   Stream& stream;
   Buffer& buffer;
   Parser parser;
   boost::url m_url;
};

// -------------------------------------------------------------------------------------------------

/**
 * Common implementation of server::Response and client::Request writer.
 */
template <typename Parent, typename Stream, typename Serializer,
          typename Message = std::remove_const_t<typename Serializer::value_type>>
   requires boost::beast::is_async_write_stream<Stream>::value
class WriterBase : public Parent
{
public:
   inline WriterBase(BeastSession<Stream>& session_, Stream& stream_) : session(&session_), stream(stream_)
   {
   }

   inline auto logPrefix() const { return session->logPrefix(); }

   // ----------------------------------------------------------------------------------------------

   const asio::any_io_executor& executor() const override { return session->executor(); }

   void detach() override { session = nullptr; }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override
   {
      if (cancelled)
      {
         mloge("async_write: already canceled");
         using namespace boost::system;
         std::move(handler)(errc::make_error_code(errc::operation_canceled));
         return;
      }

      assert(!writing);
      writing = true;

      if (buffer.size() == 0)
         mlogd("async_write: write EOF");
      else
         mlogd("async_write: {} bytes (chunked={} content_length={})", buffer.size(),
               message.chunked(), message.has_content_length());

      message.body().data = const_cast<void*>(buffer.data()); // FIXME: do we really have to cast?
      message.body().size = buffer.size();
      message.body().more = buffer.size() != 0;

      // http::response<http::buffer_body> res;
      // res.clear();

      // http::response_serializer<http::buffer_body> s;
      // s.consume(10);

      auto slot = asio::get_associated_cancellation_slot(handler);
      auto cb = [this, handler = std::move(handler)](boost::system::error_code ec,
                                                     size_t n) mutable { //
         // n is the number of bytes written to the stream
         mlogd("async_write: n={} ({}) done=({}/{})", n, ec.message(), serializer.is_header_done(),
               serializer.is_done());
         if (ec == beast::http::error::need_buffer)
            ec = {};
         else if (ec == boost::system::errc::operation_canceled)
         {
            mlogw("async_write: canceled, closing stream");
            cancelled = true;
            get_socket(stream).shutdown(boost::asio::socket_base::shutdown_send);
         }
         writing = false;
         std::move(handler)(ec);
      };

      //
      // With 'chunked' transfer encoding, the serializer will automatically emit a chunk as
      // large as possible. This means that, like the 'Cancellation' testcase, if the user writes
      // a single large buffer, cancellation can not be done gracefully at chunk boundary.
      //
      http::async_write(stream, serializer, asio::bind_cancellation_slot(slot, std::move(cb)));
   }

   // ----------------------------------------------------------------------------------------------

   /**
    * Common submit functionality for both server response and client request.
    */
   void submit_headers(const Fields& headers)
   {
      message.body().data = nullptr;

      for (auto&& header : headers)
         message.set(header.first, header.second);

      if (!message.has_content_length())
         message.chunked(true);

      mlogd("async_submit: chunked={} has_content_length={} length={}", message.chunked(),
            message.has_content_length(), message.payload_size().value_or(0));
   }

   // ----------------------------------------------------------------------------------------------

   BeastSession<Stream>* session;
   Stream& stream;
   Message message;
   Serializer serializer{message};
   bool writing = false;
   bool cancelled = false;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ResponseWriter
   : public WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>
{
   using super =
      WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>;

public:
   inline ResponseWriter(BeastSession<Stream>& session_, Stream& stream_) : super(session_, stream_) {}

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         super::message.content_length(*content_length);
      else
         super::message.content_length(boost::none);
   }

   void async_submit(WriteHandler&& handler, unsigned int status_code, Fields headers) override
   {
      super::submit_headers(headers);
      super::message.result(status_code);

      //
      // TODO: For bundling writing the header and body, we should just post the writing here,
      //       giving an async_write the change to add a body to the message first.
      //
      // post(executor(), [this](){write);
      async_write_header(
         super::stream, super::serializer,
         [handler = std::move(handler)](boost::system::error_code ec, size_t n) mutable { //
            std::move(handler)(ec);
         });
   }
};

template <typename Stream>
class RequestWriter
   : public WriterBase<client::Request::Impl, Stream, http::request_serializer<http::buffer_body>>
{
   using super =
      WriterBase<client::Request::Impl, Stream, http::request_serializer<http::buffer_body>>;

public:
   using super::logPrefix;
   using super::message;
   using super::serializer;
   using super::session;
   using super::stream;

public:
   inline RequestWriter(BeastSession<Stream>& session_, Stream& stream_) : super(session_, stream_)
   {
   }

   ~RequestWriter() override {}

   void async_submit(WriteHandler&& handler, unsigned int status_code, Fields headers) override
   {
      super::submit_headers(headers);
      message.method(http::verb::post);

      //
      // TODO: For bundling writing the header and body, we should just post the writing here,
      //       giving an async_write the chance to add a body to the message first.
      //
      async_write_header(
         stream, serializer,
         [handler = std::move(handler)](boost::system::error_code ec, size_t n) mutable { //
            std::move(handler)(ec);
         });
   }

   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      auto& buffer = session->m_buffer;
      // auto& stream = session->m_stream;

      auto reader =
         std::make_unique<BeastReader<client::Response::Impl, std::decay_t<decltype(stream)>, decltype(buffer),
                                      http::response_parser<http::buffer_body>>>(*session, stream,
                                                                                 buffer);
      http::response_parser<http::buffer_body>& parser = reader->parser;

      mlogd("waiting for response (size={} capacity={})", buffer.size(), buffer.capacity());
      async_read_header(
         stream, buffer, parser,
         [reader = std::move(reader), handler = std::move(handler),
          this](boost::system::error_code ec, size_t len) mutable
         {
            if (!ec)
            {
               http::response_parser<http::buffer_body>::value_type& msg = reader->parser.get();
               mlogd("async_read_header: len={} {} {}", len, msg.result_int(), msg.reason());
            }
            else
               mlogw("async_read_header: {} len={}", ec.message(), len);
            std::move(handler)(ec, client::Response(std::move(reader)));
         });
   }

   client::Request::GetResponseHandler responseHandler;
};

// =================================================================================================

template <typename Stream>
BeastSession<Stream>::BeastSession(std::string_view prefix, asio::any_io_executor executor,
                                   Stream&& stream)
   : m_executor(std::move(executor)), m_logPrefix(prefix), m_stream(std::move(stream))
{
   mlogd("session created");
}

template <typename Stream>
BeastSession<Stream>::~BeastSession()
{
   mlogd("session deleted");
}

template <typename Stream>
ServerSession<Stream>::ServerSession(server::Server::Impl& parent, any_io_executor executor,
                                     Stream&& stream)
   : ServerSessionBase(parent), super("\x1b[1;31mserver\x1b[0m", executor, std::move(stream))
{
}

template <typename Stream>
ClientSession<Stream>::ClientSession(client::Client::Impl& parent, any_io_executor executor,
                                     Stream&& stream)
   : ClientSessionBase(parent), super("\x1b[1;31mclient\x1b[0m", executor, std::move(stream))
{
}

template <typename Stream>
void BeastSession<Stream>::destroy()
{
   boost::system::error_code ec;
   std::ignore = get_socket(m_stream).shutdown(boost::asio::socket_base::shutdown_both, ec);
   logi("[{}] shutdown: {}", m_logPrefix, ec.message());
}

// =================================================================================================

/**
 * This function waits for headers of an incoming, new request and passes control to a registered
 * handler. After the request has been completed, abd if the connection can be kept open, it starts
 * waiting again.
 *
 * But that is only the simplified description: In reality, for pipelining support, the server
 * session may still be writing the response of a previous request when a new one arrives. The
 * queues of request and responses are processed independently of each other.
 *
 */
template <typename Stream>
awaitable<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);

   mlogd("do_server_session, {} bytes in buffer", m_buffer.size());
   get_socket(m_stream).set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout. TODO: don't rely on beast timeouts
   // m_stream.expires_after(std::chrono::seconds(30));
   // m_stream.expires_never();

   bool close = false;
   beast::error_code ec;

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
      else if (ec == http::error::end_of_stream)
         mlogd("async_read_header: end of stream");
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

      reader->m_url.clear();
      reader->m_url.set_path(request.target());

      server::Request request_wrapper(std::move(reader));

      //
      // Prepare response.
      //
      auto writer = std::make_unique<ResponseWriter<Stream>>(*this, m_stream);
      auto& response = writer->message;
      response.set(http::field::server, "anyhttp");

      server::Response response_wrapper(std::move(writer));

      //
      // Call user-provided request handler.
      //
      if (auto& handler = server().requestHandlerCoro())
      {
         try
         {
            co_await handler(std::move(request_wrapper), std::move(response_wrapper));
         }
         catch (const boost::system::system_error& e)
         {
            mloge("exception in request handler: {}", e.code().message());
            throw;
         }
      }

      //
      // FIXME: We need to wait for request and response
      //
      mlogd("request handler finished (size={} capacity={})", m_buffer.size(), m_buffer.capacity());

      continue;
      // FIXME: this is UB as request may be deleted already
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

   // FIXME: close() before shutdown()?!
   get_socket(m_stream).close();
   std::ignore = get_socket(m_stream).shutdown(asio::ip::tcp::socket::shutdown_send, ec);

   mlogd("session done");
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
awaitable<void> ClientSession<Stream>::do_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);

   mlogd("do_client_session, {} bytes in buffer", m_buffer.size());
   m_stream.socket().set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout.
   m_stream.expires_after(std::chrono::seconds(30));

   //
   // Even in HTTP/1.1, where the current request and the current response's serializers take
   // control over everything that is sent and received, we want to retain some control here,
   // on session level.
   //
   // For example, for allowing submission of multiple requests, this would be the place to take
   // text next request out of the submission queue and start writing it's headers.
   //
   // For pipelining support, we need to have a queue of pending responses and read into the
   // serializer of the front element.
   //

   //
   // TODO: There should be something that keeps the session alive. We are just waiting for
   //       the client to make a request here right now...
   //
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

template <typename Stream>
void ServerSession<Stream>::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                         Fields headers)
{
   assert(false);
}

template <typename Stream>
void ClientSession<Stream>::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                         Fields headers)
{
   mlogd("submit: {}", url.buffer());

   auto writer = std::make_unique<RequestWriter<Stream>>(*this, m_stream);
   auto& request = writer->message;

   request.base().target(url.path());
   request.method(http::verb::post);
   request.set(http::field::user_agent, "anyhttp");
   for (auto&& header : headers)
      request.set(header.first, header.second);
   if (!request.has_content_length())
      request.chunked(true);

   //
   // TODO: make writer shared? put into queue
   //
   async_write_header(m_stream, writer->serializer,
                      [handler = std::move(handler), writer = std::move(writer)](
                         boost::system::error_code ec, size_t n) mutable { //
                         std::move(handler)(boost::system::error_code{},
                                            client::Request(std::move(writer)));
                      });
   ;
}

// =================================================================================================

template class ClientSession<boost::beast::tcp_stream>;
template class ServerSession<boost::beast::tcp_stream>;
template class ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>;

} // namespace anyhttp::beast_impl
