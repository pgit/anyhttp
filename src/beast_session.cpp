#include "anyhttp/beast_session.hpp"
#include "anyhttp/any_async_stream.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/server.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/core/buffer_traits.hpp>
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
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/errc.hpp>

#include <boost/url/parse.hpp>

#include <string_view>

using namespace std::chrono_literals;

using namespace boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace errc = boost::system::errc;

namespace anyhttp::beast_impl
{

// =================================================================================================

using namespace asio;
using namespace boost::beast;
using socket = asio::ip::tcp::socket;

inline auto& get_socket(socket& socket) { return socket; }
inline auto& get_socket(tcp_stream& stream) { return stream.socket(); }
inline auto& get_socket(ssl::stream<socket>& stream) { return stream.lowest_layer(); }
inline auto& get_socket(AnyAsyncStream& stream) { return stream.get_socket(); }

// =================================================================================================

template <typename Interface, typename Stream, typename Buffer, typename Parser>
class BeastReader : public Interface
{
public:
   inline BeastReader(BeastSession<Stream>& session_, Stream& stream_, Buffer& buffer_)
      : session(&session_), stream(stream_), buffer(buffer_)
   {
      parser.body_limit(std::numeric_limits<uint64_t>::max());
   }

   void destroy() noexcept override
   {
      if (!parser.is_done() && session)
      {
         logw("destroy: reader destroyed, but parser not done yet... closing socket");
         //
         // We could still try to send an error response here.
         // This breaks WHEN_server_discards_request_THEN_is_still_able_to_deliver_response.
         //
         error_code ec;
         // get_socket(stream).shutdown(boost::asio::socket_base::shutdown_send, ec);
         get_socket(stream).shutdown(boost::asio::socket_base::shutdown_receive, ec);
         session->m_closed = true;
         if (ec)
            logw("destroy: shutdown: {}", what(ec));
      }
   }

   ~BeastReader() override
   {
      assert(!reading);
      if (session)
         session->rx = nullptr;
   }
   void detach() override
   {
      mlogw("detach");
      session = nullptr;
   }

   unsigned int status_code() const noexcept override
   {
      if constexpr (typename Parser::is_request())
         return 0;
      else
         return parser.get().result_int();
   }
   boost::url_view url() const override { return m_url; }
   std::optional<size_t> content_length() const noexcept override
   {
      if (parser.content_length())
         return parser.content_length().value();
      else
         return std::nullopt;
   }

   void async_read_some(asio::mutable_buffer body_buffer, ReadSomeHandler&& handler) override
   {
      buffer.reserve(64 * 1024);
      mlogd("async_read_some: is_done={} size={} capacity={}", parser.is_done(), buffer.size(),
            buffer.capacity());

      assert(!reading);

      if (body_buffer.size() == 0 || parser.is_done())
      {
         any_completion_executor ex = get_associated_immediate_executor(handler, get_executor());
         ex.execute([handler = std::move(handler)]() mutable { //
            std::move(handler)(boost::system::error_code{}, 0);
         });
         return;
      }

      reading = true;
      parser.get().body().data = body_buffer.data();
      parser.get().body().size = body_buffer.size();

      auto cs = asio::get_associated_cancellation_slot(handler);
      auto cb = [this, self = Interface::shared_from_this(), body_buffer = std::move(body_buffer),
                 handler = std::move(handler)](boost::system::error_code ec, size_t n) mutable
      {
         reading = false;

         auto& body = parser.get().body();
         size_t payload = body_buffer.size() - body.size;
         mlogd("async_read_some: n={} (body={}) ({}) is_done={} size={} capacity={}", n, payload,
               ec.message(), parser.is_done(), buffer.size(), buffer.capacity());
         if (ec == beast::http::error::need_buffer)
            ec = {}; // FIXME: maybe we should keep 'need_buffer' to avoid extra empty round trip

         if (!ec && payload == 0)
            async_read_some(body_buffer, std::move(handler));
         else
            std::move(handler)(ec, payload);
      };

      //
      // TODO: Manually forwarding the cancellation slot fixes per-operation cancellation. But
      //       there are other handler traits (executor, allocator) that might need forwarding.
      //       Instead of doing this, we should try to use async_compose<>, which seems to do
      //       that automatically.
      //
      //  Note that beast::http::async_read_same() is implemented using async_compose<>, too.
      //
      boost::beast::http::async_read_some(stream, buffer, parser,
                                          asio::bind_cancellation_slot(cs, std::move(cb)));
   }

   asio::any_io_executor get_executor() const noexcept { return session->get_executor(); }
   inline auto logPrefix() const { return session ? session->logPrefix() : "DETACHED"; }

   BeastSession<Stream>* session;
   Stream& stream;
   Buffer& buffer;
   Parser parser;
   std::optional<unsigned int> m_status_code = 0;
   boost::url m_url;
   bool reading = false;
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
   inline WriterBase(BeastSession<Stream>& session_, Stream& stream_)
      : session(&session_), stream(stream_)
   {
   }

   ~WriterBase() override
   {
      assert(!writing);
      if (session)
         session->wx = nullptr;
   }

   inline auto logPrefix() const { return session ? session->logPrefix() : "DETACHED"; }

   // ----------------------------------------------------------------------------------------------

   asio::any_io_executor get_executor() const noexcept override { return session->get_executor(); }

   void detach() override
   {
      mlogw("detach");
      session = nullptr;
   }

   template <typename Handler, typename... Args>
      requires std::invocable<Handler, Args...>
   inline void complete_immediately(Handler&& handler, Args&&... args)
   {
      auto ex = asio::get_associated_immediate_executor(handler, get_executor());
      ex.execute([handler = std::forward<Handler>(handler)] mutable { //
         std::move(handler)(errc::make_error_code(errc::operation_canceled));
      });
   }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override
   {
      if (cancelled)
      {
         mloge("async_write: already canceled");
         complete_immediately(std::move(handler), errc::make_error_code(errc::operation_canceled));
         return;
      }

      logd("async_write: {} bytes", buffer.size());

      assert(!writing);
      writing = true;

      if (buffer.size() == 0)
         mlogd("async_write: write EOF");
      else
         mlogd("async_write: {} bytes (chunked={} content_length={})", buffer.size(),
               message.chunked(), message.has_content_length());

      // https://github.com/boostorg/beast/issues/3032
      // make sure to set 'nullptr' on empty size, otherwise beast may serialize an empty chunk
      message.body().data = buffer.size() ? const_cast<void*>(buffer.data()) : nullptr;
      message.body().size = buffer.size();
      message.body().more = buffer.size() != 0; // empty buffer --> EOF

      auto slot = asio::get_associated_cancellation_slot(handler);
      auto cb = [this, self = Parent::shared_from_this(), expected = buffer.size(),
                 handler = std::move(handler)] //
         (boost::system::error_code ec, size_t n) mutable
      {
         // async op result 'n' is the number of bytes written to the stream,
         // not the number of bytes read from the buffer
         mlogd("async_write: n={} (\x1b[1;{}m{}\x1b[0m) done={} (body {})", n,
               ec == beast::http::error::need_buffer ? 33 : 31, // need_buffer in yellow only
               ec.message(), serializer.is_done(), serializer.get().body().size);

         writing = false;

         //
         // 'need_buffer' means that the serializer is done consuming all of the given buffer
         // and is ready to accept a new one.
         //
         if (ec == beast::http::error::need_buffer)
            ec = {};
         else if (ec == errc::operation_canceled)
         {
            //
            // Cancellation is tricky, see e.g.: https://github.com/boostorg/beast/issues/2325.
            //
            // Main reason is that, depending on when the cancellation actually takes place,
            // the stream is in an undefined state. For example, when writing a large chunk is
            // interrupted, there is no meaningful way to recover: The length of the chunk has
            // been written, but only part of the data.
            //
            // So the only sensible thing to do here is to close the socket.
            //
            // TODO: We could try to support partial cancellation, but that would only work
            //       at chunk boundaries.
            //
            mlogw("async_write: canceled after writing {} of {} bytes", n, expected);
            cancelled = true;
            mlogw("async_write: canceled, closing stream");
            get_socket(stream).shutdown(boost::asio::socket_base::shutdown_send);
         }
         else if (ec)
         {
            cancelled = true;
         }
         /*
         else if (!ec && n < expected)
         {
            mlogw("async_write: wrote {} bytes which is less than expected ({})", n, expected);
            ec = errc::make_error_code(errc::message_size);
         }
         */

         std::move(handler)(ec);
      };

      //
      // With 'chunked' transfer encoding, the serializer will automatically emit a chunk as
      // large as possible. This means that if the user writes a single large buffer, cancellation
      // can not be done gracefully at chunk boundary any more. See 'Cancellation' testcase for an
      // example of this.
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
         message.set(header.name_string(), header.value());

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
   bool response_requested = false;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ResponseWriter
   : public WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>
{
   using super =
      WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>;

public:
   using super::message;
   using super::serializer;
   using super::stream;
   using super::submit_headers;

public:
   inline ResponseWriter(BeastSession<Stream>& session_, Stream& stream_) : super(session_, stream_)
   {
   }

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         message.content_length(*content_length);
      else
         message.content_length(boost::none);
   }

   void async_submit(WriteHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      message.result(status_code);

      if (message.find(http::field::date) == message.end())
         message.set(http::field::date, format_http_date(std::chrono::system_clock::now()));

      submit_headers(headers);

      if (message.find(http::field::date) == message.end())
         message.set(http::field::server, "anyhttp");

      //
      // TODO: For bundling writing the header and body, we should just post the writing here,
      //       giving an async_write the change to add a body to the message first.
      //
      // post(get_executor(), [this](){write);
      async_write_header(
         stream, serializer,
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
   using super::response_requested;
   using super::serializer;
   using super::session;
   using super::stream;
   using super::submit_headers;

public:
   inline RequestWriter(BeastSession<Stream>& session_, Stream& stream_) : super(session_, stream_)
   {
   }

   ~RequestWriter() = default;

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         message.content_length(*content_length);
      else
         message.content_length(boost::none);
   }

   asio::any_io_executor get_executor() const noexcept { return session->get_executor(); }

   void async_submit(WriteHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      submit_headers(headers);
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
      logd("async_get_response:");

      if (response_requested)
      {
         auto ec = asio::error::basic_errors::already_started;
         logw("async_get_response: \x1b[1;31m{}\x1b[0m", what(ec));
         any_completion_executor ex = get_associated_immediate_executor(handler, get_executor());
         ex.execute([handler = std::move(handler), ec = std::move(ec)]() mutable { //
            std::move(handler)(ec, client::Response{nullptr});
         });
         return;
      }
      response_requested = true;

      auto& buffer = session->m_buffer;
      // auto& stream = session->m_stream;

      auto reader =
         std::make_unique<BeastReader<client::Response::Impl, std::decay_t<decltype(stream)>,
                                      decltype(buffer), http::response_parser<http::buffer_body>>>(
            *session, stream, buffer);
      session->rx = reader.get();
      http::response_parser<http::buffer_body>& parser = reader->parser;

      auto slot = get_associated_cancellation_slot(handler);
      auto intermediate = [reader = std::move(reader), handler = std::move(handler),
                           this](boost::system::error_code ec, size_t len) mutable
      {
         if (!ec)
         {
            http::response_parser<http::buffer_body>::value_type& msg = reader->parser.get();
            mlogd("async_read_header: len={} {} {}", len, msg.result_int(), msg.reason());
         }
         else
            mlogw("async_read_header: {} len={}", ec.message(), len);

         //
         // If reading the headers was cancelled before receiving anything, we can allow another
         // attempt. TODO: If we move the parser into the session, we can even relax this further.
         //
         if (ec == errc::operation_canceled && !reader->parser.got_some())
            response_requested = false;

         std::move(handler)(ec, client::Response(std::move(reader)));
      };

      mlogd("waiting for response (size={} capacity={})", buffer.size(), buffer.capacity());
      async_read_header(stream, buffer, parser,
                        bind_cancellation_slot(slot, std::move(intermediate)));
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
   if (wx)
   {
      mlogw("dtor: detaching writer");
      wx->detach();
   }
   if (rx)
   {
      mlogw("dtor: detaching reader");
      rx->detach();
   }
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
   : ClientSessionBase(parent), super("\x1b[1;32mclient\x1b[0m", executor, std::move(stream))
{
}

template <typename Stream>
void BeastSession<Stream>::destroy() noexcept
{
   //
   // FIXME: ClientAsync.Cancellation runs into a heap-use-after-free here, when the session is
   //        deleted. This is because the request and response may outlive the session and are
   //        not properly detached.
   //

   // post(get_executor(), [this, self]() mutable {
   boost::system::error_code ec;
   std::ignore = get_socket(m_stream).shutdown(socket_base::shutdown_both, ec);
   logwi(ec, "[{}] destroy: socket shutdown: {}", m_logPrefix, ec.message());
   // });
}

// =================================================================================================

/**
 * This function waits for headers of an incoming, new request and passes control to a registered
 * handler. After the request has been completed, and if the connection can be kept open, it starts
 * waiting again.
 *
 * But that is only the simplified description: In reality, for pipelining support, the server
 * session may still be writing the response of a previous request when a new one arrives. The
 * queues of request and responses are processed independently of each other.
 *
 * And even without pipelining, for structuring concurrency, we want to clean up existing request
 * and response objects when the sessions ends.
 *
 */
template <typename Stream>
awaitable<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);

   mlogd("do_server_session, {} bytes in buffer", m_buffer.size());
   // get_socket(m_stream).set_option(asio::ip::tcp::no_delay(true));

   // Set the timeout. TODO: don't rely on beast timeouts
   // m_stream.expires_after(std::chrono::seconds(5));
   // m_stream.expires_never();

   bool close = false;
   beast::error_code ec;

   size_t requestCounter = 0;
   while (!m_closed)
   {
      auto reader =
         std::make_unique<BeastReader<server::Request::Impl, decltype(m_stream), decltype(m_buffer),
                                      http::request_parser<http::buffer_body>>>(*this, m_stream,
                                                                                m_buffer);
      if (rx)
         rx->detach();
      rx = reader.get();

      logd("");
      mlogd("waiting for request (size={} capacity={})", m_buffer.size(), m_buffer.capacity());
      auto& parser = reader->parser;
      auto [ec, len] = co_await async_read_header(m_stream, m_buffer, parser, as_tuple);
      if (!ec)
         mlogd("async_read_header: len={} size={} capacity={} ec={}", len, m_buffer.size(),
               m_buffer.capacity(), ec.message());
      else if (ec == http::error::end_of_stream)
         mlogd("async_read_header: end of stream");
      else
         mlogw("async_read_header: len={} size={} capacity={} ec=\x1b[1;31m{}\x1b[0m", len,
               m_buffer.size(), m_buffer.capacity(), ec.message());
      if (ec)
         break;
      requestCounter++;

      auto& request = parser.get();
      const bool need_eof = request.need_eof();
      mlogd("{} {} (need_eof={})", request.method_string(), request.target(), request.need_eof());
      for (auto& header : request)
         mlogd("  \x1b[1;34m{}\x1b[0m: {}", header.name_string(), header.value());

      // if (auto url = boost::urls::parse_relative_ref(request.target()); url.has_value())
      if (auto url = boost::urls::parse_uri_reference(request.target()); url.has_value())
         reader->m_url = url.value();
      else
         mlogw("{} {}: invalid target: {}", request.method_string(), request.target(),
               url.error().message());

      //
      // Deduce scheme from underlying socket type.
      //
      if constexpr (std::is_same_v<Stream, asio::ssl::stream<asio::ip::tcp::socket>>)
         reader->m_url.set_scheme("https");
      else
         reader->m_url.set_scheme("http");

      try
      {
         reader->m_url.set_encoded_authority(request[http::field::host]);
      }
      catch (std::exception& ex)
      {
         mlogw("ignoring invalid host header: {}", request[http::field::host]);
      }

      //
      // Prepare response.
      //
      auto writer = std::make_unique<ResponseWriter<Stream>>(*this, m_stream);
      if (wx)
         wx->detach();
      wx = writer.get();

      http::response<http::buffer_body>& response = writer->message;
      http::response_serializer<http::buffer_body>& serializer = writer->serializer;
      response.set(http::field::server, "anyhttp");

      //
      // Call user-provided request handler.
      //
      // Unlike HTTP2, the request handler is not co_spawn()ed as a separate thread of execution,
      // because HTTP/1.1 does not do multiplexing.
      //
      // TODO: If we really want to attempt this, for pipelining, reading new requests and
      //       serializing responses needs to be decoupled. Then, we would have a queue of
      //       incoming requests and and another one of outgoing responses, which could make
      //       progress independently (at least to a certain degree).
      //
      server::Request request_wrapper(std::move(reader));
      server::Response response_wrapper(std::move(writer));
      if (auto& handler = server().requestHandlerCoro())
      {
         try
         {
            co_await handler(std::move(request_wrapper), std::move(response_wrapper));
         }
         catch (const boost::system::system_error& e)
         {
            mloge("exception in request handler: {}", e.code().message());
            get_socket(m_stream).shutdown(socket_base::shutdown_both);
            throw;
         }
      }

      //
      // FIXME: Maybe we shouldn't hand out put the parser object to the request handler. If the
      //        request gets dropped, we don't know anything about the stream's state any more and
      //        whether or not we can try to read a new request.
      //
      //        We should have a parser here, and call is_done() on it.
      //
      mlogd("request handler finished (size={} capacity={})", m_buffer.size(), m_buffer.capacity());

      //
      // Honor 'Connection: close'
      //
      if (need_eof)
      {
         mlogd("request needs EOF, closing connection");
         break;
      }

      /*
      // FIXME: this is UB as request/response may be deleted already
      if (response.need_eof())
      {
         mlogd("response needs EOF, closing connection");
         break;
      }
         */
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
   // get_socket(m_stream).set_option(asio::ip::tcp::no_delay(true));

   // Set the low-level TCP stream timeout. This is relevant for some testcases...
   m_stream.expires_after(5s);

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
   // But even for cancellation only, when the client is destroyed while there is still a pending
   // request, we need to have a way to inform the request that the session is gone.
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
   timer.expires_after(2s);
   co_await timer.async_wait(deferred);

   // auto [ec, len] = co_await async_read_header(m_stream, buffer, parser, as_tuple(deferred));
   co_return;
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
void ServerSession<Stream>::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                         const Fields& headers)
{
   assert(false);
}

template <typename Stream>
void ClientSession<Stream>::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                         const Fields& headers)
{
   mlogd("submit: {}", url.buffer());

   auto writer = std::make_unique<RequestWriter<Stream>>(*this, m_stream);
   wx = writer.get();
   auto& request = writer->message;

   request.base().target(url.encoded_target());
   request.method(http::verb::post);
   request.set(http::field::user_agent, "anyhttp");
   for (auto&& header : headers)
      request.set(header.name_string(), header.value());
   if (request.find(http::field::host) == request.end())
      request.set(http::field::host, url.encoded_authority());
   if (!request.has_content_length())
      request.chunked(true);

   //
   // TODO: make writer shared? put into queue
   //
   auto& serializer = writer->serializer;
   async_write_header(m_stream, serializer,
                      [handler = std::move(handler), writer = std::move(writer), this](
                         boost::system::error_code ec, size_t n) mutable { //
                         std::move(handler)(std::move(ec),
                                            client::Request(std::move(writer)));
                      });
   ;
}

// =================================================================================================

template class ClientSession<boost::beast::tcp_stream>;
template class ServerSession<boost::beast::tcp_stream>;
template class ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>;
template class ServerSession<AnyAsyncStream>;

} // namespace anyhttp::beast_impl
