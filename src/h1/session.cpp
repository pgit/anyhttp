#include "anyhttp/h1/session.hpp"

#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h1/backend.hpp"
#include "anyhttp/h1/io.hpp"
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"
#include "anyhttp/server.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/rfc7230.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/version.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/errc.hpp>

#include <boost/url/parse.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <string_view>

using namespace std::chrono_literals;

using namespace boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace anyhttp::beast_impl
{

// =================================================================================================

using namespace asio;
using namespace boost::beast;

/**
 * Adds the user's header fields to an outgoing message. A field replaces whatever the message
 * already has under that name, like a default set before, but repeated fields are all kept.
 */
template <bool isRequest, typename Body>
static void add_fields(http::message<isRequest, Body>& message, const Fields& headers)
{
   for (auto&& header : headers)
      message.erase(header.name_string());
   for (auto&& header : headers)
      message.insert(header.name_string(), header.value());
}

/// Converts Config::max_header_size into what a Beast parser takes as its header limit.
static std::uint32_t header_limit(size_t max_header_size)
{
   return static_cast<std::uint32_t>(
      std::min<size_t>(max_header_size, std::numeric_limits<std::uint32_t>::max()));
}

// =================================================================================================

namespace
{

template <typename Interface, typename Stream, typename Buffer, typename Parser>
class BeastReader : public Interface
{
public:
   BeastReader(BeastSession<Stream>& session_, Stream& stream_, Buffer& buffer_)
      : session(&session_), stream(stream_), buffer(buffer_),
        executor_(session_.get_executor()), // survives detach(), see get_executor()
        log_prefix_(session_.log_prefix())
   {
      // parser.header_limit(std::numeric_limits<uint32_t>::max());
      parser.body_limit(std::numeric_limits<uint64_t>::max());
      session_.attach(*this);
   }

   void destroy() noexcept override
   {
      mlogd("destroy: reader destroyed, is_done={}", parser.is_done());
      if (!parser.is_done() && session)
      {
         mlogw("destroy: reader destroyed, but parser not done yet... closing socket");
         //
         // We could still try to send an error response here.
         // This breaks WHEN_server_discards_request_THEN_is_still_able_to_deliver_response.
         //
         auto ec = io::shutdown(stream, io::Shutdown::receive);
         session->closed_ = true;
         logwd(ec != errc::not_connected, "[{}] destroy: shutdown: {}", log_prefix(), what(ec));
      }
      finish();
   }

   ~BeastReader() override
   {
      assert(!reading);
      finish();
      if (session)
         session->release(*this);
   }

   /// Tells the session, once, that this reader is done with the stream, see reader_finished().
   void finish()
   {
      if (session && !finished)
      {
         finished = true;
         session->reader_finished(parser.is_done());
      }
   }
   void detach() override
   {
      mlogd("detach");
      session = nullptr;
   }

   const Fields& fields() const override { return parser.get(); }
   std::optional<size_t> content_length() const noexcept override
   {
      if (parser.content_length())
         return parser.content_length().value();
      else
         return std::nullopt;
   }

   //
   // The I/O is done in a coroutine, see Reader::Impl::read_some(). As a task of its own, it is
   // what the caller's cancellation reaches.
   //
   void async_read_some(asio::mutable_buffer body_buffer, ReadSomeHandler&& handler) override
   {
      launch(get_executor(), read_some(body_buffer), std::move(handler));
   }

   /// Reads until something of the body has arrived in \p body_buffer, or the body has ended.
   Task<std::tuple<error_code, size_t>> read_some(asio::mutable_buffer body_buffer) override
   {
      assert(!reading);

      //
      // Everything that can be answered without touching the connection, each case with the error
      // code the Reader contract prescribes for it (see common.hpp): a zero-length read is not a
      // read and reports nothing, wherever the body stands; a parser that is done keeps reporting
      // the end of the body, ASIO-style, session or no session; and a read past a detached,
      // unfinished parser is a truncation the caller must hear about.
      //
      if (body_buffer.size() == 0)
         co_return std::tuple{error_code{}, size_t{0}};
      else if (parser.is_done())
         co_return std::tuple{errors::eof, size_t{0}};
      else if (!session)
         co_return std::tuple{errors::partial_message, size_t{0}};

      buffer.reserve(64_k); // the buffer is the session's, so not before checking for it
      mlogd("async_read_some: is_done={} size={} capacity={}", parser.is_done(), buffer.size(),
            buffer.capacity());

      auto self = Interface::shared_from_this();
      reading = true;
      for (;;)
      {
         parser.get().body().data = body_buffer.data();
         parser.get().body().size = body_buffer.size();

         error_code ec;
         size_t n = 0;
         try
         {
            std::tie(ec, n) = co_await h1::read_some(stream, buffer, parser);
         }
         catch (const system_error& e) // a cancelled coroutine throws from its next co_await
         {
            ec = e.code();
         }

         auto& body = parser.get().body();
         size_t payload = body_buffer.size() - body.size;
         mlogd("async_read_some: n={} (body={}) ({}) is_done={} size={} capacity={}", n, payload,
               ec.message(), parser.is_done(), buffer.size(), buffer.capacity());

         if (parser.is_done())
            finish();

         //
         // Nothing of the body came out of this round -- either the parser is done now, which
         // is the end of the body, or it just needs more input. Either way there is nothing to
         // hand to the caller yet.
         //
         if (!ec && payload == 0)
         {
            if (parser.is_done())
               ec = errors::eof;
            else if (!session)
               ec = errors::partial_message;
            else
               continue;
         }

         reading = false;
         co_return std::tuple{ec, payload};
      }
   }

   Executor get_executor() const noexcept override { return executor_; }
   std::string log_prefix() const override { return log_prefix_; }

   BeastSession<Stream>* session;
   Stream& stream;
   Buffer& buffer;
   Parser parser;
   Executor executor_; // kept as a copy so a detached reader can still complete
   std::string log_prefix_; // likewise, for logging
   bool reading = false;
   bool finished = false; // see finish()
};

// -------------------------------------------------------------------------------------------------

//
// The two roles a reader can be in. Everything above is the same for both; what they add is the
// half of the incoming message that only their role has -- a request line, or a status code.
// Which of the two Beast parsers a role reads with follows from the role itself.
//

template <typename Stream, typename Buffer>
class BeastRequestReader final
   : public BeastReader<server::Request::Impl, Stream, Buffer,
                        beast::http::request_parser<beast::http::buffer_body>>
{
   using Base = BeastReader<server::Request::Impl, Stream, Buffer,
                            beast::http::request_parser<beast::http::buffer_body>>;

public:
   using Base::Base;

   std::string_view method() const noexcept override { return this->parser.get().method_string(); }
   boost::url_view url() const override { return url_; }

   /// Assembled from the request target, the host header and the kind of socket this arrived on,
   /// by the session, right after the header was parsed.
   boost::url url_;
};

template <typename Stream, typename Buffer>
class BeastResponseReader final
   : public BeastReader<client::Response::Impl, Stream, Buffer,
                        beast::http::response_parser<beast::http::buffer_body>>
{
   using Base = BeastReader<client::Response::Impl, Stream, Buffer,
                            beast::http::response_parser<beast::http::buffer_body>>;

public:
   using Base::Base;

   unsigned int status_code() const noexcept override { return this->parser.get().result_int(); }
};

// -------------------------------------------------------------------------------------------------

/**
 * Common implementation of server::Response and client::Request writer.
 */
template <typename Parent, typename Stream, typename Serializer,
          typename Message = std::remove_const_t<typename Serializer::value_type>>
class WriterBase : public Parent
{
public:
   WriterBase(BeastSession<Stream>& session_, Stream& stream_)
      : session(&session_), stream(stream_),
        executor_(session_.get_executor()), // survives detach(), see get_executor()
        log_prefix_(session_.log_prefix())
   {
      session_.attach(*this);
   }

   ~WriterBase() override
   {
      assert(!writing);
      if (session)
         session->release(*this);
   }

   std::string log_prefix() const override { return log_prefix_; }

   // ----------------------------------------------------------------------------------------------

   Executor get_executor() const noexcept override { return executor_; }

   void detach() override
   {
      mlogd("detach");
      session = nullptr;
   }

   /// The I/O is done in a coroutine, see Reader::Impl::read_some() and BeastReader.
   void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) override
   {
      launch(get_executor(), write(buffer, eof), std::move(handler));
   }

   Task<std::tuple<error_code>> write(asio::const_buffer buffer, bool eof) override
   {
      const bool empty = buffer.size() == 0;

      //
      // The protocol-independent entry ladder, in the order the Writer contract in common.hpp
      // prescribes: a zero-length non-EOF write is a free no-op wherever the body stands (it
      // would otherwise turn into an empty chunk); after the body has ended, data has no body
      // left to belong to -- through either entry point -- while a bare re-end is idempotent.
      // Only then does it matter that the session, and with it the stream, may be gone.
      //
      if (empty && !eof)
         co_return std::tuple{error_code{}};

      if (eof_submitted)
      {
         if (!empty)
            mloge("async_write: body has already been ended");
         co_return std::tuple{empty ? error_code{} : make_error_code(errc::broken_pipe)};
      }

      if (!session)
      {
         mlogw("async_write: session already gone");
         co_return std::tuple{errors::connection_aborted};
      }

      if (cancelled)
      {
         mloge("async_write: already canceled");
         co_return std::tuple{errors::canceled};
      }

      assert(!writing);
      writing = true;

      mlogd("async_write: {} bytes (eof={} chunked={} content_length={})", buffer.size(), eof,
            message.chunked(), message.has_content_length());

      //
      // The last body buffer and the end of the body go into the same serializer pass: with
      // 'more' cleared, beast emits the data and the terminating chunk (or just the data, for a
      // content-length delimited body) in one go, so ending a body costs no extra write.
      //
#if BOOST_BEAST_VERSION < 359
      // https://github.com/boostorg/beast/issues/3032
      // make sure to set 'nullptr' on empty size, otherwise beast may serialize an empty chunk
      message.body().data = buffer.size() ? const_cast<void*>(buffer.data()) : nullptr;
#else
      message.body().data = const_cast<void*>(buffer.data());
#endif
      message.body().size = buffer.size();
      message.body().more = !eof;

      //
      // With 'chunked' transfer encoding, the serializer will automatically emit a chunk as
      // large as possible. This means that if the user writes a single large buffer, cancellation
      // can not be done gracefully at chunk boundary any more. See 'Cancellation' testcase for an
      // example of this.
      //
      auto self = Parent::shared_from_this();
      const auto expected = buffer.size();

      // 'n' is the number of bytes written to the stream, not the number taken from the buffer
      error_code ec;
      size_t n = 0;
      try
      {
         std::tie(ec, n) = co_await h1::write(stream, serializer);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }
      mlogd("async_write: n={} (\x1b[1;{}m{}\x1b[0m) done={} (body {})", n, ec ? 31 : 32,
            ec.message(), serializer.is_done(), serializer.get().body().size);

      writing = false;

      if (ec == errc::operation_canceled)
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
         mlogi("async_write: canceled after writing {} of {} bytes", n, expected);
         cancelled = true;
         if (session) // otherwise, the stream is gone already
         {
            mlogi("async_write: canceled, closing stream");
            io::shutdown(stream, io::Shutdown::send);
         }
      }
      else if (ec)
      {
         cancelled = true;
      }
      /*
      else if (!ec && n < expected)
      {
         mlogw("async_write: wrote {} bytes which is less than expected ({})", n, expected);
         ec = make_error_code(errc::message_size);
      }
      */

      //
      // Only now is the body really ended: a cancelled or failed EOF write never got its
      // terminating bytes onto the wire, and latching the flag at accept time would let a
      // retried async_write_eof() report success for a body the peer sees as truncated.
      //
      if (!ec && eof)
         eof_submitted = true;

      if (session && eof_submitted)
         body_ended();
      else if (session && cancelled)
         write_failed();

      //
      // The caller may be resumed right after this. If it releases the writer, that has to take
      // effect immediately, not only when this task is gone.
      //
      self.reset();
      co_return std::tuple{ec};
   }

   /// Called when a write has ended the body, before its handler is invoked.
   virtual void body_ended() {}

   /// Called when a write has failed, leaving the stream unusable for this message.
   virtual void write_failed() {}

   // ----------------------------------------------------------------------------------------------

   /// Writes the header of the message, see submit_headers().
   Task<std::tuple<error_code>> write_head()
   {
      error_code ec;
      try
      {
         std::tie(ec, std::ignore) = co_await h1::write_header(stream, serializer);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }
      co_return std::tuple{ec};
   }

   /**
    * Common submit functionality for both server response and client request.
    */
   void submit_headers(const Fields& headers)
   {
      message.body().data = nullptr;

      add_fields(message, headers);

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
   Executor executor_; // kept as a copy so a detached writer can still complete
   std::string log_prefix_; // likewise, for logging
   bool writing = false;
   bool cancelled = false;
   bool response_requested = false;
   bool eof_submitted = false;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ResponseWriter
   : public WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>
{
   using super =
      WriterBase<server::Response::Impl, Stream, http::response_serializer<http::buffer_body>>;

public:
   using super::cancelled;
   using super::log_prefix;
   using super::message;
   using super::serializer;
   using super::session;
   using super::stream;
   using super::submit_headers;

public:
   ResponseWriter(BeastSession<Stream>& session_, Stream& stream_) : super(session_, stream_) {}

   //
   // A response that could not be written leaves the connection unusable, so the session must not
   // go on to whatever the client has pipelined behind this request. With a reset connection, the
   // next writes would not even fail: after a couple of failed sends, ASIO's epoll reactor waits
   // for the socket to become writable, which a dead socket signals only once.
   //
   void write_failed() override { session->closed_ = true; }

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         message.content_length(*content_length);
      else
         message.content_length(boost::none);
   }

   /// The I/O is done in a coroutine, see BeastReader.
   void async_submit(StatusHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      launch(super::get_executor(), submit(status_code, headers), std::move(handler));
   }

   Task<std::tuple<error_code>> submit(unsigned int status_code, Fields headers) override
   {
      if (!session)
      {
         mlogw("async_submit: session already gone");
         co_return std::tuple{errors::connection_aborted};
      }

      message.result(status_code);

      if (message.find(http::field::date) == message.end())
         message.set(http::field::date, format_http_date(std::chrono::system_clock::now()));

      submit_headers(headers);

      if (message.find(http::field::server) == message.end())
         message.set(http::field::server, "anyhttp");

      mlogd("{} {}", message.result_int(), message.reason());
      for (const auto& header : message)
         mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name_string()),
               truncated(header.value()));

      //
      // TODO: For bundling writing the header and body, we should just post the writing here,
      //       giving an async_write the chance to add a body to the message first.
      //
      auto [ec] = co_await super::write_head();
      if (ec)
      {
         mlogd("async_submit: {}", what(ec));
         cancelled = true; // nothing more of this response is going to reach the stream
         if (session)
            write_failed();
      }
      co_return std::tuple{ec};
   }
};

} // namespace

template <typename Stream>
class RequestWriter
   : public WriterBase<client::Request::Impl, Stream, http::request_serializer<http::buffer_body>>
{
   using super =
      WriterBase<client::Request::Impl, Stream, http::request_serializer<http::buffer_body>>;

public:
   using super::cancelled;
   using super::eof_submitted;
   using super::log_prefix;
   using super::message;
   using super::response_requested;
   using super::serializer;
   using super::session;
   using super::stream;
   using super::submit_headers;

public:
   RequestWriter(ClientSession<Stream>& session_, Stream& stream_) : super(session_, stream_) {}

   ~RequestWriter() override
   {
      if (session)
         client_session().request_released(*this);
   }

   ClientSession<Stream>& client_session()
   {
      assert(session);
      return static_cast<ClientSession<Stream>&>(*session);
   }

   // ----------------------------------------------------------------------------------------------

   /**
    * Whether the request has a body, going by its framing (RFC 9112, section 6.3): a request
    * without 'Transfer-Encoding' and without a 'Content-Length' other than zero has none. As
    * ClientSession::async_submit() makes every request without 'Content-Length' chunked, that
    * leaves "Content-Length: 0". The request method plays no part in this.
    */
   bool has_body() const
   {
      if (message.chunked())
         return true;

      auto value = message[http::field::content_length];
      size_t length = 0;
      auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
      return ec != std::errc{} || end != value.data() + value.size() || length != 0;
   }

   /// Called when writing the header is done. A request without a body is complete by then.
   void header_written(error_code ec)
   {
      if (ec)
      {
         mlogw("async_submit: {}", what(ec));
         cancelled = true;
         if (session)
            client_session().request_failed(*this);
      }
      else if (!has_body())
      {
         eof_submitted = true;
         if (session)
            client_session().request_complete(*this);
      }
   }

   void body_ended() override { client_session().request_complete(*this); }
   void write_failed() override { client_session().request_failed(*this); }

   void content_length(std::optional<size_t> content_length) override
   {
      if (content_length)
         message.content_length(*content_length);
      else
         message.content_length(boost::none);
   }

   using super::get_executor;

   void async_submit(StatusHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      launch(get_executor(), submit_head(headers), std::move(handler));
   }

   Task<std::tuple<error_code>> submit_head(Fields headers)
   {
      if (!session)
      {
         mlogw("async_submit: session already gone");
         co_return std::tuple{errors::connection_aborted};
      }

      submit_headers(headers);
      message.method(http::verb::post);
      co_return co_await super::write_head();
   }

   /// The I/O is done in a coroutine, see BeastReader.
   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      launch(get_executor(), get_response(), std::move(handler));
   }

   Task<std::tuple<error_code, client::Response>> get_response() override
   {
      mlogd("async_get_response:");

      if (response_requested)
      {
         auto ec = errors::already_started;
         mlogw("async_get_response: \x1b[1;31m{}\x1b[0m", what(ec));
         co_return std::tuple{ec, client::Response{}};
      }

      if (!session)
      {
         mlogw("async_get_response: session already gone");
         co_return std::tuple{errors::connection_aborted, client::Response{}};
      }

      //
      // Responses arrive in the order the requests were sent. Reading the response to this one
      // has to wait until the responses to all earlier requests have been read. Instead of
      // waiting, this is an error. See ClientSession for details.
      //
      auto& cs = client_session();
      error_code ec;
      if (cs.receive_failed_)
         ec = errors::connection_aborted;
      else if (sequence != cs.responses_read_)
         ec = errors::would_block;
      if (ec)
      {
         mlogw("async_get_response: {} (request #{}, {} responses read)", what(ec), sequence,
               cs.responses_read_);
         co_return std::tuple{ec, client::Response{}};
      }
      response_requested = true;

      auto& buffer = session->buffer_;
      auto reader =
         std::make_unique<BeastResponseReader<std::decay_t<decltype(stream)>, decltype(buffer)>>(
            *session, stream, buffer);
      auto& parser = reader->parser;
      parser.header_limit(header_limit(cs.client().config().max_header_size));

      mlogd("waiting for response (size={} capacity={})", buffer.size(), buffer.capacity());
      size_t len = 0;
      try
      {
         std::tie(ec, len) = co_await h1::read_header(stream, buffer, parser);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }

      if (!ec)
      {
         auto& msg = parser.get();
         mlogd("{} {}", msg.result_int(), msg.reason());
         for (const auto& header : msg)
            mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name_string()),
                  truncated(header.value()));

         //
         // An "Alt-Svc" on any response may point at an HTTP/3 endpoint to use for the next
         // connection (RFC 7838), see Client::Impl::on_alt_svc().
         //
         if (auto alt_svc = msg[http::field::alt_svc]; session && !alt_svc.empty())
            client_session().client().on_alt_svc(std::string_view(alt_svc));
      }
      else
         mlogw("async_read_header: {} len={}", ec.message(), len);

      //
      // If reading the headers was cancelled before receiving anything, we can allow another
      // attempt. TODO: If we move the parser into the session, we can even relax this further.
      //
      // As this reader has not taken anything from the connection, it does not count as having
      // failed to read the response, either.
      //
      if (ec == errc::operation_canceled && !parser.got_some())
      {
         response_requested = false;
         reader->finished = true;
      }

      if (!ec && parser.is_done()) // a response without body is complete already
         reader->finish();

      co_return std::tuple{ec, client::Response(std::move(reader))};
   }

   client::Request::GetResponseHandler responseHandler;

   /// Position of this request on the connection, which is also the position of its response.
   size_t sequence = 0;
};

// =================================================================================================

template <typename Stream>
BeastSession<Stream>::BeastSession(std::string_view prefix, Executor executor, Stream&& stream)
   : executor_(std::move(executor)), log_prefix_(prefix), stream_(std::move(stream))
{
   mlogd("session created");
}

template <typename Stream>
BeastSession<Stream>::~BeastSession()
{
   mlogd("session deleted");
   if (!writers_.empty())
      mlogi("dtor: detaching {} writer(s)", writers_.size());
   detach_writers();
   if (!readers_.empty())
      mlogi("dtor: detaching {} reader(s)", readers_.size());
   detach_readers();
}

template <typename Stream>
ServerSession<Stream>::ServerSession(server::Server::Impl& parent, Executor executor,
                                     Stream&& stream)
   : ServerSessionBase(parent),
     super(anyhttp::log_prefix(Role::server, "h1", io::remote_endpoint(stream)),
           std::move(executor), std::move(stream))
{
}

template <typename Stream>
ClientSession<Stream>::ClientSession(client::Client::Impl& parent, Executor executor,
                                     Stream&& stream)
   : ClientSessionBase(parent),
     super(anyhttp::log_prefix(Role::client, "h1", io::remote_endpoint(stream)),
           std::move(executor), std::move(stream))
{
}

template <typename Stream>
void BeastSession<Stream>::destroy() noexcept
{
   mlogd("destroy: closing stream");
   //
   // FIXME: ClientAsync.Cancellation runs into a heap-use-after-free here, when the session is
   //        deleted. This is because the request and response may outlive the session and are
   //        not properly detached.
   //

   // post(get_executor(), [this, self]() mutable {
   auto ec = io::shutdown(stream_, io::Shutdown::both);
   // not_connected: the peer is gone already, which is what we wanted anyway
   logwd(ec && ec != errc::not_connected, //
         "[{}] destroy: socket shutdown: {}", log_prefix_, ec.message());
   // });
}

template <typename Stream>
void ServerSession<Stream>::destroy() noexcept
{
   if (upgraded_)
      upgraded_->destroy(); // the stream has been moved there
   else
   {
      super::destroy();

      //
      // Shutting the socket down does not end what is pending on it in every case: on a
      // connection the peer has reset, it fails with not_connected, and a write the reactor has
      // queued may be waiting for a readiness that never comes (see ResponseWriter). Neither
      // should the requests still in the buffer be served.
      //
      io::cancel(stream_);
      closed_ = true;
   }
}

// =================================================================================================

/**
 * Returns what the HTTP/2 session needs to continue \p request as stream 1, if the request asks
 * for an upgrade to h2c (RFC 7540, section 3.2) and it can be granted. Otherwise, the upgrade is
 * ignored and the request is served as HTTP/1.1, which is always a valid response to it.
 *
 * Only requests that are complete after their header are upgraded: A request body would have to
 * be read in HTTP/1.1 first, before switching protocols. Cleartext only, as h2 over TLS is
 * negotiated by ALPN instead.
 */
static std::optional<nghttp2::Upgrade> h2c_upgrade(std::string_view log_prefix,
                                                   const http::request<http::buffer_body>& request,
                                                   const boost::urls::url& url, bool complete)
{
   const auto has_token = [](std::string_view list, std::string_view token) {
      for (auto item : http::token_list(list))
         if (beast::iequals(item, token))
            return true;
      return false;
   };

   if (!has_token(request[http::field::upgrade], "h2c") || url.scheme() != "http")
      return std::nullopt;

   if (!has_token(request[http::field::connection], "upgrade") ||
       !has_token(request[http::field::connection], "http2-settings"))
   {
      logw("[{}] upgrade: ignoring h2c upgrade, 'Connection' misses 'Upgrade' or 'HTTP2-Settings'",
           log_prefix);
      return std::nullopt;
   }

   // exactly one HTTP2-Settings header, containing base64url without padding
   if (request.count("HTTP2-Settings") != 1)
   {
      logw("[{}] upgrade: ignoring h2c upgrade, need exactly one 'HTTP2-Settings' header",
           log_prefix);
      return std::nullopt;
   }

   if (!complete)
   {
      logw("[{}] upgrade: ignoring h2c upgrade for request with body", log_prefix);
      return std::nullopt;
   }

   std::string encoded(request["HTTP2-Settings"]);
   std::ranges::replace(encoded, '-', '+');
   std::ranges::replace(encoded, '_', '/');

   nghttp2::Upgrade upgrade;
   upgrade.settings.resize(beast::detail::base64::decoded_size(encoded.size()));
   auto [written, read] =
      beast::detail::base64::decode(upgrade.settings.data(), encoded.data(), encoded.size());
   upgrade.settings.resize(written);

   // a SETTINGS payload is a sequence of 6-byte entries
   if (read != encoded.size() || upgrade.settings.size() % 6 != 0)
   {
      logw("[{}] upgrade: ignoring h2c upgrade, invalid 'HTTP2-Settings' header", log_prefix);
      return std::nullopt;
   }

   upgrade.method = request.method_string();
   upgrade.url = url;

   //
   // HTTP/2 has no connection-specific header fields (RFC 9113, section 8.2.2), and the upgrade
   // ones are used up by now.
   //
   for (const auto& field : request)
   {
      switch (field.name())
      {
      case http::field::connection:
      case http::field::proxy_connection:
      case http::field::keep_alive:
      case http::field::transfer_encoding:
      case http::field::upgrade:
      case http::field::http2_settings:
         break;
      default:
         upgrade.fields.insert(field.name_string(), field.value());
      }
   }
   return upgrade;
}

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
Task<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   buffer_ = std::move(buffer);

   mlogd("do_server_session, {} bytes in buffer", buffer_.size());

   size_t requestCounter = 0;
   while (!closed_)
   {
      detach_readers(); // the previous request, if still around, is done with the stream
      auto reader = std::make_unique<BeastRequestReader<decltype(stream_), decltype(buffer_)>>(
         *this, stream_, buffer_);

      logd("");
      mlogd("waiting for request (size={} capacity={})", buffer_.size(), buffer_.capacity());
      auto& parser = reader->parser;
      parser.header_limit(header_limit(server().config().max_header_size));
      auto [ec, len] = co_await h1::read_header(stream_, buffer_, parser);
      if (!ec)
         mlogd("async_read_header: len={} size={} capacity={} ec={}", len, buffer_.size(),
               buffer_.capacity(), ec.message());
      else if (ec == h1::to_error_code(http::error::end_of_stream))
         mlogd("async_read_header: end of stream");
      else
         mlogw("async_read_header: len={} size={} capacity={} ec=\x1b[1;31m{}\x1b[0m", len,
               buffer_.size(), buffer_.capacity(), ec.message());

      //
      // The rest of the request can not be told apart from whatever follows it on the connection,
      // so there is nothing left to do after telling the client why.
      //
      if (ec == errors::header_limit)
      {
         http::response<http::empty_body> res{http::status::request_header_fields_too_large, 11};
         res.set(http::field::server, "anyhttp");
         res.set(http::field::connection, "close");
         res.content_length(0);
         if (auto [ec, n] = co_await h1::write_message(stream_, res); ec)
            mlogw("writing 431 response: {}", ec.message());
      }
      if (ec)
         break;
      requestCounter++;

      auto& request = parser.get();
      const bool need_eof = request.need_eof();

      // if (auto url = boost::urls::parse_relative_ref(request.target()); url.has_value())
      if (auto url = boost::urls::parse_uri_reference(request.target()); url.has_value())
         reader->url_ = url.value();
      else
         mlogw("{} {}: invalid target: {}", request.method_string(), request.target(),
               url.error().message());

      //
      // Deduce scheme from underlying socket type.
      // Other than that, HTTP does not have a way to convey a custom "scheme".
      // Only in HTTP/2 and HTTP/3 there is a pseudo-header for that.
      //
      // https://datatracker.ietf.org/doc/html/rfc7230#section-5.3
      //
      if (reader->url_.has_scheme())
         ; // keep it
      else if (is_tls(stream_))
         reader->url_.set_scheme("https");
      else
         reader->url_.set_scheme("http");

      try
      {
         reader->url_.set_encoded_authority(request[http::field::host]);
      }
      catch (std::exception& ex)
      {
         mlogw("ignoring invalid host header: {}", request[http::field::host]);
      }

      mlogd("{} {} (need_eof={})", request.method_string(), reader->url_.buffer(), need_eof);
      for (auto& header : request)
         mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name_string()),
               truncated(header.value()));

      //
      // Upgrade to h2c, if requested: Answer with "101 Switching Protocols" and hand over the
      // stream to an HTTP/2 session, which continues this request as stream 1. Anything that
      // follows in the buffer (the client preface) is already HTTP/2.
      //
      if (auto upgrade = h2c_upgrade(log_prefix(), request, reader->url_, parser.is_done()))
      {
         http::response<http::empty_body> res{http::status::switching_protocols, request.version()};
         res.set(http::field::connection, "Upgrade");
         res.set(http::field::upgrade, "h2c");
         reader.reset(); // owns the parser and thereby 'request'

         if (auto [ec, n] = co_await h1::write_message(stream_, res); ec)
         {
            mlogw("upgrade: writing 101 response: {}", ec.message());
            break;
         }

         mlogi("upgrading to h2c, {} bytes in buffer", buffer_.size());
         // Stream is whatever this session runs on, but never a TLS one: h2c_upgrade() takes
         // cleartext requests only, as h2 over TLS is negotiated by ALPN instead.
         upgraded_ = nghttp2::make_server_session(server(), super::get_executor(),
                                                  std::move(stream_), std::move(*upgrade));
         co_await upgraded_->do_session(std::move(buffer_));
         mlogi("h2c session done, served {} requests before upgrade", requestCounter - 1);
         co_return;
      }

      //
      // Prepare response.
      //
      detach_writers(); // likewise for the previous response
      auto writer = std::make_shared<ResponseWriter<Stream>>(*this, stream_);

      http::response<http::buffer_body>& response = writer->message;
      http::response_serializer<http::buffer_body>& serializer = writer->serializer;
      response.set(http::field::server, "anyhttp");

      //
      // Point the client at our HTTP/3 endpoint, see server::Config::alt_svc_max_age. Set before
      // the handler runs, so one that wants to say something else about alternative services can
      // simply pass its own field: submitting a response replaces the fields it names.
      //
      if (const auto& alt_svc = server().alt_svc(); !alt_svc.empty())
         response.set(http::field::alt_svc, alt_svc);

      //
      // A client that asked for the connection to end -- or one speaking HTTP/1.0, which has no
      // persistent connections unless it asks for one -- gets one last response, and that response
      // has to say that it is the last one (RFC 9112, section 9.6): without it, the client can not
      // tell the end of the connection from one that was lost mid-message.
      //
      if (need_eof)
         response.keep_alive(false);

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
      if (auto& handler = server().request_handler())
      {
         try
         {
            co_await handler(std::move(request_wrapper), std::move(response_wrapper));
         }
         catch (const system_error& e)
         {
            mloge("exception in request handler: {}", e.code().message());
            io::shutdown(stream_, io::Shutdown::both);
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
      mlogd("request handler finished (size={} capacity={})", buffer_.size(), buffer_.capacity());

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

   //
   // End the stream itself first: over TLS, that is the "close_notify" the peer needs to tell the
   // end of the data from a connection that was cut. Everything else has nothing to send here.
   //
   if (auto teardown_ec = co_await async_teardown(stream_); teardown_ec)
      mlogw("teardown: {}", teardown_ec.message());

   //
   // Send a FIN next, and let go of the socket only once the peer has ended its side as well
   // (RFC 9112, section 9.6), reading and dropping what still comes in for two seconds at most:
   // see io::drain() why.
   //
   if (auto ec = io::shutdown(stream_, io::Shutdown::send); !ec)
   {
      if (auto drained = co_await io::drain(stream_, super::get_executor(), 2s))
         mlogd("dropped {} bytes the peer sent after the last request", drained);
   }
   else if (ec != errc::not_connected) // the peer may be gone already
      mlogw("shutdown: {}", ec.message());
   io::close(stream_);

   mlogd("session done");
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
Task<void> ClientSession<Stream>::do_session(Buffer&& buffer)
{
   buffer_ = std::move(buffer);

   mlogd("do_client_session, {} bytes in buffer", buffer_.size());

   // Set the low-level TCP stream timeout. This is relevant for some testcases...
   // stream_.expires_after(5s);

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
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
void ServerSession<Stream>::async_submit(SubmitHandler&& handler, std::string_view method,
                                         boost::urls::url url, const Fields& headers)
{
   std::ignore = handler;
   std::ignore = method;
   std::ignore = url;
   std::ignore = headers;
   assert(false);
}

/// The I/O is done in a coroutine, see BeastReader.
template <typename Stream>
void ClientSession<Stream>::async_submit(SubmitHandler&& handler, std::string_view method,
                                         boost::urls::url target, const Fields& headers)
{
   launch(super::get_executor(), submit(std::string(method), std::move(target), headers),
          std::move(handler));
}

template <typename Stream>
Task<std::tuple<error_code, client::Request>>
ClientSession<Stream>::submit(std::string method, boost::urls::url target, Fields headers)
{
   //
   // Only one request can be incomplete at a time, see ClientSession. Instead of waiting for the
   // previous one, which might never happen if the caller is the one to complete it, this is an
   // error.
   //
   error_code ec;
   if (send_failed_)
      ec = errors::connection_aborted;
   else if (sending_)
      ec = errors::would_block;
   if (ec)
   {
      mlogw("async_submit: {} ({})", what(ec),
            sending_ ? "previous request not complete yet" : "an earlier request failed");
      co_return std::tuple{ec, client::Request{}};
   }

   auto writer = std::make_unique<RequestWriter<Stream>>(*this, stream_);
   auto& request = writer->message;

   //
   // FIXME: https://datatracker.ietf.org/doc/html/rfc7230#section-5.3
   //        Usually, target should be in "origin form", but we need to support other forms, too.
   //
   request.base().target(target.encoded_target());
   // request.base().target(url.buffer());
   request.method_string(method);
   request.set(http::field::user_agent, "anyhttp");
   add_fields(request, headers);
   if (request.find(http::field::host) == request.end())
      request.set(http::field::host, target.encoded_host_and_port());
   if (!request.has_content_length())
      request.chunked(true);

   mlogd("{} {}", request.method_string(), target.buffer());
   for (const auto& header : request)
      mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name_string()),
            truncated(header.value()));

   writer->sequence = requests_sent_++;
   sending_ = writer.get();

   std::tie(ec) = co_await writer->write_head();
   writer->header_written(ec);
   co_return std::tuple{ec, client::Request(std::move(writer))};
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
void ClientSession<Stream>::request_complete(RequestWriter<Stream>& request)
{
   assert(sending_ == &request);
   mlogd("request #{} complete", request.sequence);
   sending_ = nullptr;
}

template <typename Stream>
void ClientSession<Stream>::request_failed(RequestWriter<Stream>& request)
{
   mlogw("request #{} failed, no more requests can be sent", request.sequence);
   send_failed_ = true;
   sending_ = nullptr;
}

template <typename Stream>
void ClientSession<Stream>::request_released(RequestWriter<Stream>& request)
{
   //
   // The connection is in the middle of this request, and nothing else can be sent any more.
   //
   if (sending_ == &request)
      request_failed(request);

   //
   // Its response will be coming, but nobody is going to read it.
   //
   if (!request.response_requested)
   {
      mlogw("request #{} released without getting its response", request.sequence);
      receive_failed_ = true;
   }
}

template <typename Stream>
void ClientSession<Stream>::reader_finished(bool complete)
{
   if (complete)
      ++responses_read_;
   else
   {
      mlogw("response #{} not read completely", responses_read_);
      receive_failed_ = true;
   }
}

// =================================================================================================

#define ANYHTTP_H1_SESSION(Stream) template class ServerSession<Stream>;
ANYHTTP_SERVER_STREAMS(ANYHTTP_H1_SESSION)
#undef ANYHTTP_H1_SESSION

// =================================================================================================
// Factories, see anyhttp/h1/backend.hpp. Instantiating the session templates is kept to this
// translation unit, so that the generic server and client stay free of beast's HTTP machinery.
// =================================================================================================

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server, Executor executor,
                                                   Stream&& stream)
{
   return std::make_shared<ServerSession<Stream>>(server, std::move(executor), std::move(stream));
}

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client, Executor executor,
                                                   Stream&& stream)
{
   return std::make_shared<ClientSession<Stream>>(client, std::move(executor), std::move(stream));
}

#define ANYHTTP_H1_SERVER(Stream)                                                                  \
   template std::shared_ptr<Session::Impl> make_server_session<Stream>(server::Server::Impl&,      \
                                                                       Executor, Stream&&);
#define ANYHTTP_H1_CLIENT(Stream)                                                                  \
   template std::shared_ptr<Session::Impl> make_client_session<Stream>(client::Client::Impl&,      \
                                                                       Executor, Stream&&);
ANYHTTP_SERVER_STREAMS(ANYHTTP_H1_SERVER)
ANYHTTP_CLIENT_STREAMS(ANYHTTP_H1_CLIENT)
#undef ANYHTTP_H1_SERVER
#undef ANYHTTP_H1_CLIENT

// =================================================================================================

} // namespace anyhttp::beast_impl
