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

#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/request.hpp>
#include <boost/http/response.hpp>
#include <boost/http/rfc/list_rule.hpp>
#include <boost/http/rfc/token_rule.hpp>
#include <boost/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/errc.hpp>

#include <boost/url/grammar/ci_string.hpp>
#include <boost/url/grammar/parse.hpp>
#include <boost/url/parse.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string_view>

using namespace std::chrono_literals;

namespace anyhttp::h1
{

// =================================================================================================

using namespace boost::asio;
using http::field;

/**
 * Adds the user's header fields to an outgoing message. A field replaces whatever the message
 * already has under that name, like a default set before, but repeated fields are all kept.
 */
static void add_fields(http::message_base& message, const Fields& headers)
{
   for (auto&& header : headers)
      message.erase(header.name);
   for (auto&& header : headers)
      message.append(header.name, header.value);
}

/// A copy of the header fields of \p message, which stay with the parser only until the next one.
static Fields copy_fields(const http::fields_base& message)
{
   Fields fields;
   fields.reserve_bytes(message.buffer().size());
   for (auto&& header : message)
      fields.append(header.name, header.value);
   return fields;
}

/// Whether \p message has a 'Content-Length', valid or not.
static bool has_content_length(const http::message_base& message)
{
   return message.metadata().content_length.count > 0;
}

/// Sets the 'Content-Length' of \p message, or removes it.
static void set_content_length(http::message_base& message, std::optional<size_t> content_length)
{
   if (content_length)
      message.set_content_length(*content_length);
   else
      message.erase(field::content_length);
}

// =================================================================================================

namespace
{

/**
 * The incoming half of a message, as a Reader. The parser is the session's, see H1Session: its
 * header is good until the next message, so the reader keeps a copy of what it hands out. It reads
 * through the parser while it is attached, and not at all any more once it is detached.
 */
template <typename Interface, typename Stream, typename Parser>
class H1Reader : public Interface
{
public:
   H1Reader(H1Session<Stream>& session_, Parser& parser_)
      : session(&session_), parser(&parser_),
        executor_(session_.get_executor()), // survives detach(), see get_executor()
        log_prefix_(session_.log_prefix())
   {
      session_.attach(*this);
   }

   void destroy() noexcept override
   {
      mlogd("destroy: reader destroyed, complete={}", complete);
      if (!complete && session)
      {
         mlogw("destroy: reader destroyed, but parser not done yet... closing socket");
         //
         // We could still try to send an error response here.
         // This breaks WHEN_server_discards_request_THEN_is_still_able_to_deliver_response.
         //
         auto ec = io::shutdown(session->stream_, io::Shutdown::receive);
         session->closed_ = true;
         logwd(ec != errc::not_connected, "[{}] destroy: shutdown: {}", log_prefix(), what(ec));
      }
      finish();
   }

   ~H1Reader() override
   {
      assert(!reading);
      finish();
      if (session)
         session->release(*this);
   }

   /// Takes what is needed of the header, once the parser has it.
   void on_header()
   {
      const auto& message = parser->get();
      fields_ = copy_fields(message);

      //
      // What the message announced as its body length. A response to HEAD has no payload whatever
      // its 'Content-Length' says, but that is still what it announced, and what HTTP/2 and HTTP/3
      // report for it.
      //
      const auto& content_length = message.metadata().content_length;
      if (message.payload() == http::payload::size)
         content_length_ = message.payload_size();
      else if (content_length.count && !content_length.ec)
         content_length_ = content_length.value;
      else if (message.payload() == http::payload::none)
         content_length_ = 0;
      complete = parser->is_complete();
   }

   /// Tells the session, once, that this reader is done with the stream, see reader_finished().
   void finish()
   {
      if (session && !finished)
      {
         finished = true;
         session->reader_finished(complete);
      }
   }
   void detach() override
   {
      mlogd("detach");
      session = nullptr;
      parser = nullptr;
   }

   const Fields& fields() const override { return fields_; }
   std::optional<size_t> content_length() const noexcept override { return content_length_; }

   //
   // The I/O is done in a coroutine, see Reader::Impl::read_some(). As a task of its own, it is
   // what the caller's cancellation reaches.
   //
   void async_read_some(boost::asio::mutable_buffer body_buffer, ReadSomeHandler&& handler) override
   {
      launch(get_executor(), read_some(body_buffer), std::move(handler));
   }

   /// Reads until something of the body has arrived in \p body_buffer, or the body has ended.
   IoTask<size_t> read_some(boost::asio::mutable_buffer body_buffer) override
   {
      assert(!reading);

      //
      // Everything that can be answered without touching the connection, each case with the error
      // code the Reader contract prescribes for it (see common.hpp): a zero-length read is not a
      // read and reports nothing, wherever the body stands; a message that has been read keeps
      // reporting the end of the body, ASIO-style, session or no session; and a read past a
      // detached, unfinished one is a truncation the caller must hear about.
      //
      if (body_buffer.size() == 0)
         co_return std::tuple{error_code{}, size_t{0}};
      else if (eof)
         co_return std::tuple{errors::eof, size_t{0}};
      else if (!session)
         co_return std::tuple{complete ? errors::eof : errors::partial_message, size_t{0}};

      auto self = Interface::shared_from_this();
      reading = true;

      error_code ec;
      size_t n = 0;
      try
      {
         const auto op = session->read_op();
         std::tie(ec, n) =
            co_await h1::read_body(session->stream_, session->pending_, *parser, body_buffer);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }

      if (parser) // still attached
         complete = parser->is_complete();
      if (ec == errors::eof)
         eof = true;
      mlogd("async_read_some: n={} ({}) complete={}", n, ec.message(), complete);

      if (complete)
         finish();

      reading = false;
      co_return std::tuple{ec, n};
   }

   Executor get_executor() const noexcept override { return executor_; }
   std::string log_prefix() const override { return log_prefix_; }

   H1Session<Stream>* session;
   Parser* parser; ///< the session's, see on_header()
   Fields fields_;
   std::optional<size_t> content_length_;
   Executor executor_; // kept as a copy so a detached reader can still complete
   std::string log_prefix_; // likewise, for logging
   bool reading = false;
   bool complete = false; ///< whether the parser has all of the message
   bool eof = false; ///< whether all of the body has been read, too
   bool finished = false; // see finish()
};

// -------------------------------------------------------------------------------------------------

//
// The two roles a reader can be in. Everything above is the same for both; what they add is the
// half of the incoming message that only their role has -- a request line, or a status code.
// Which of the two parsers a role reads with follows from the role itself.
//

template <typename Stream>
class RequestReader final : public H1Reader<server::Request::Impl, Stream, http::request_parser>
{
   using Base = H1Reader<server::Request::Impl, Stream, http::request_parser>;

public:
   using Base::Base;

   std::string_view method() const noexcept override { return method_; }
   boost::url_view url() const override { return url_; }

   /// Copied from the request line, see H1Reader::on_header().
   std::string method_;

   /// Assembled from the request target, the host header and the kind of socket this arrived on,
   /// by the session, right after the header was parsed.
   boost::url url_;
};

template <typename Stream>
class ResponseReader final : public H1Reader<client::Response::Impl, Stream, http::response_parser>
{
   using Base = H1Reader<client::Response::Impl, Stream, http::response_parser>;

public:
   using Base::Base;

   unsigned int status_code() const noexcept override { return status_code_; }

   unsigned int status_code_ = 0;
};

// -------------------------------------------------------------------------------------------------

/**
 * Common implementation of server::Response and client::Request writer. The message is the
 * writer's own, the serializer the session's: there is only one message at a time to write.
 */
template <typename Parent, typename Stream, typename Message>
class WriterBase : public Parent
{
public:
   explicit WriterBase(H1Session<Stream>& session_)
      : session(&session_),
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

   /// The I/O is done in a coroutine, see Reader::Impl::read_some() and H1Reader.
   void async_write(WriteHandler&& handler, boost::asio::const_buffer buffer, bool eof) override
   {
      launch(get_executor(), write(buffer, eof), std::move(handler));
   }

   IoTask<size_t> write(boost::asio::const_buffer buffer, bool eof) override
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
         co_return std::tuple{error_code{}, size_t{0}};

      if (eof_submitted)
      {
         if (!empty)
            mloge("async_write: body has already been ended");
         co_return std::tuple{empty ? error_code{} : make_error_code(errc::broken_pipe), size_t{0}};
      }

      if (!session)
      {
         mlogw("async_write: session already gone");
         co_return std::tuple{errors::connection_aborted, size_t{0}};
      }

      if (cancelled)
      {
         mloge("async_write: already canceled");
         co_return std::tuple{errors::canceled, size_t{0}};
      }

      assert(!writing);
      writing = true;

      mlogd("async_write: {} bytes (eof={} chunked={} content_length={})", buffer.size(), eof,
            message.metadata().transfer_encoding.is_chunked, has_content_length(message));

      //
      // The last body buffer and the end of the body go into the same serializer pass, so ending
      // a body costs no extra write: the data and the terminating chunk (or just the data, for a
      // content-length delimited body) go out together.
      //
      // With 'chunked' transfer encoding, the serializer makes a chunk of as much as its buffer
      // holds. So cancellation can not be done gracefully at a chunk boundary of the caller's.
      // See 'Cancellation' testcase for an example of this.
      //
      auto self = Parent::shared_from_this();
      const auto expected = buffer.size();

      // 'taken' is how much of the buffer the serializer has copied, see h1::write_body()
      error_code ec;
      size_t taken = 0;
      try
      {
         const auto op = session->write_op();
         std::tie(ec, taken) =
            co_await h1::write_body(session->stream_, session->serializer_, buffer, eof);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }
      mlogd("async_write: taken={} (\x1b[1;{}m{}\x1b[0m)", taken, ec ? 31 : 32, ec.message());

      writing = false;

      if (ec == errc::operation_canceled)
      {
         //
         // Cancellation is tricky: depending on when the cancellation actually takes place, the
         // stream is in an undefined state. For example, when writing a large chunk is
         // interrupted, there is no meaningful way to recover: The length of the chunk has
         // been written, but only part of the data.
         //
         // So the only sensible thing to do here is to close the socket.
         //
         // TODO: We could try to support partial cancellation, but that would only work
         //       at chunk boundaries.
         //
         mlogi("async_write: canceled after taking {} of {} bytes", taken, expected);
         cancelled = true;
         if (session) // otherwise, the stream is gone already
         {
            mlogi("async_write: canceled, closing stream");
            io::shutdown(session->stream_, io::Shutdown::send);
         }
      }
      else if (ec)
      {
         cancelled = true;
      }

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
      co_return std::tuple{ec, ec ? taken : expected};
   }

   /// Called when a write has ended the body, before its handler is invoked.
   virtual void body_ended() {}

   /// Called when a write has failed, leaving the stream unusable for this message.
   virtual void write_failed() {}

   // ----------------------------------------------------------------------------------------------

   /// Writes the header of the message, see submit_headers(). With \p body cleared, that is all.
   IoTask<> write_head(bool body = true)
   {
      error_code ec;
      try
      {
         const auto op = session->write_op();
         std::tie(ec) =
            co_await h1::write_header(session->stream_, session->serializer_, message, body);
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
      add_fields(message, headers);

      if (!has_content_length(message))
         message.set_chunked(true);

      mlogd("async_submit: chunked={} has_content_length={} length={}",
            message.metadata().transfer_encoding.is_chunked, has_content_length(message),
            message.metadata().content_length.value);
   }

   /// Logs the header section of the message, start line excluded.
   void log_fields() const
   {
      for (const auto& header : message)
         mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name), truncated(header.value));
   }

   // ----------------------------------------------------------------------------------------------

   H1Session<Stream>* session;
   Message message;
   Executor executor_; // kept as a copy so a detached writer can still complete
   std::string log_prefix_; // likewise, for logging
   bool writing = false;
   bool cancelled = false;
   bool response_requested = false;
   bool eof_submitted = false;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ResponseWriter : public WriterBase<server::Response::Impl, Stream, http::response>
{
   using super = WriterBase<server::Response::Impl, Stream, http::response>;

public:
   using super::cancelled;
   using super::log_prefix;
   using super::message;
   using super::session;
   using super::submit_headers;

public:
   explicit ResponseWriter(H1Session<Stream>& session_) : super(session_) {}

   //
   // A response that could not be written leaves the connection unusable, so the session must not
   // go on to whatever the client has pipelined behind this request. With a reset connection, the
   // next writes would not even fail: after a couple of failed sends, ASIO's epoll reactor waits
   // for the socket to become writable, which a dead socket signals only once.
   //
   void write_failed() override { session->closed_ = true; }

   void content_length(std::optional<size_t> content_length) override
   {
      set_content_length(message, content_length);
   }

   /// The I/O is done in a coroutine, see H1Reader.
   void async_submit(StatusHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      launch(super::get_executor(), submit(status_code, headers), std::move(handler));
   }

   IoTask<> submit(unsigned int status_code, Fields headers) override
   {
      if (!session)
      {
         mlogw("async_submit: session already gone");
         co_return std::tuple{errors::connection_aborted};
      }

      const auto status = static_cast<unsigned short>(status_code);
      message.set_start_line(status, reason_phrase(status));

      if (!message.exists(field::date))
         message.set(field::date, std::string_view(http_date_now()));

      submit_headers(headers);

      //
      // A response that says "close" is the last one on the connection, whether the client asked
      // for it or the handler decided so (RFC 9112, section 9.6): the session ends after it, and
      // whatever the client has pipelined behind this request goes unserved. Latched here, as the
      // response may be gone by the time the handler returns.
      //
      if (!message.keep_alive())
         session->closed_ = true;

      if (!message.exists(field::server))
         message.set(field::server, "anyhttp");

      mlogd("{} {}", message.status_int(), message.reason());
      super::log_fields();

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
class RequestWriter : public WriterBase<client::Request::Impl, Stream, http::request>
{
   using super = WriterBase<client::Request::Impl, Stream, http::request>;

public:
   using super::cancelled;
   using super::eof_submitted;
   using super::log_prefix;
   using super::message;
   using super::response_requested;
   using super::session;
   using super::submit_headers;

public:
   explicit RequestWriter(ClientSession<Stream>& session_) : super(session_) {}

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
      const auto& metadata = message.metadata();
      if (metadata.transfer_encoding.count)
         return true;
      if (!metadata.content_length.count)
         return false;
      return metadata.content_length.ec || metadata.content_length.value != 0;
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
      set_content_length(message, content_length);
   }

   using super::get_executor;

   void async_submit(StatusHandler&& handler, unsigned int status_code,
                     const Fields& headers) override
   {
      launch(get_executor(), submit_head(headers), std::move(handler));
   }

   IoTask<> submit_head(Fields headers)
   {
      if (!session)
      {
         mlogw("async_submit: session already gone");
         co_return std::tuple{errors::connection_aborted};
      }

      submit_headers(headers);
      message.set_method(http::method::post);
      co_return co_await super::write_head(has_body());
   }

   /// The I/O is done in a coroutine, see H1Reader.
   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      launch(get_executor(), get_response(), std::move(handler));
   }

   IoTask<client::Response> get_response() override
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

      //
      // The parser is the session's. It is done with the previous response, or has not seen one
      // yet, unless an earlier attempt to read this one was cancelled, see below: then it goes on
      // where that one left off.
      //
      auto& parser = cs.parser_;
      if (!std::exchange(cs.parser_started_, true))
      {
         // whatever its header field says, a response to HEAD is over after that (RFC 9112, 6.3)
         if (message.method() == http::method::head)
            parser.start_head_response();
         else
            parser.start();
      }

      auto reader = std::make_unique<ResponseReader<Stream>>(*session, parser);

      mlogd("waiting for response ({} bytes pending)", session->pending_.size());
      try
      {
         const auto op = session->read_op();
         std::tie(ec) = co_await h1::read_header(session->stream_, session->pending_, parser);
      }
      catch (const system_error& e) // a cancelled coroutine throws from its next co_await
      {
         ec = e.code();
      }

      if (!ec)
      {
         const auto& msg = parser.get();
         reader->status_code_ = msg.status_int();
         reader->on_header();
         mlogd("{} {}", msg.status_int(), msg.reason());
         for (const auto& header : msg)
            mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name), truncated(header.value));

         //
         // An "Alt-Svc" on any response may point at an HTTP/3 endpoint to use for the next
         // connection (RFC 7838), see Client::Impl::on_alt_svc().
         //
         if (auto alt_svc = msg.value_or(field::alt_svc, ""); session && !alt_svc.empty())
            client_session().client().on_alt_svc(std::string_view(alt_svc));
      }
      else
         mlogw("async_read_header: {}", ec.message());

      //
      // If reading the header was cancelled before it was complete, another attempt can pick up
      // where this one left off: the parser is the session's, and keeps what it has got so far.
      //
      // As this reader has not taken anything from the connection that the next one would not
      // take, it does not count as having failed to read the response, either.
      //
      if (ec == errc::operation_canceled && !parser.got_header())
      {
         response_requested = false;
         reader->finished = true;
      }
      else
         cs.parser_started_ = false;

      if (!ec && reader->complete) // a response without body is complete already
         reader->finish();

      co_return std::tuple{ec, client::Response(std::move(reader))};
   }

   client::Request::GetResponseHandler responseHandler;

   /// Position of this request on the connection, which is also the position of its response.
   size_t sequence = 0;
};

// =================================================================================================

template <typename Stream>
H1Session<Stream>::H1Session(std::string_view prefix, Executor executor, Stream&& stream)
   : log_prefix_(prefix), executor_(std::move(executor)), stream_(std::move(stream)),
     serializer_(h1::make_serializer_config())
{
   mlogd("session created");
}

template <typename Stream>
H1Session<Stream>::~H1Session()
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
           std::move(executor), std::move(stream)),
     parser_(h1::make_parser_config(true, parent.config().max_header_size))
{
   parser_.reset();
}

template <typename Stream>
ClientSession<Stream>::ClientSession(client::Client::Impl& parent, Executor executor,
                                     Stream&& stream)
   : ClientSessionBase(parent),
     super(anyhttp::log_prefix(Role::client, "h1", io::remote_endpoint(stream)),
           std::move(executor), std::move(stream)),
     parser_(h1::make_parser_config(false, parent.config().max_header_size))
{
   parser_.reset();
}

template <typename Stream>
Task<void> H1Session<Stream>::end_connection(bool notify)
{
   //
   // End the stream itself first: over TLS, that is the "close_notify" the peer needs to tell the
   // end of the data from a connection that was cut. Everything else has nothing to send here.
   //
   if (notify)
   {
      if (auto ec = co_await async_teardown(stream_); ec)
         mlogw("teardown: {}", ec.message());
   }

   //
   // Send a FIN next, and let go of the socket only once the peer has ended its side as well
   // (RFC 9112, section 9.6), reading and dropping what still comes in for two seconds at most:
   // see io::drain() why.
   //
   if (auto ec = io::shutdown(stream_, io::Shutdown::send); !ec)
   {
      if (auto drained = co_await io::drain(stream_, this->weak_from_this(), get_executor(), 2s))
         mlogd("dropped {} bytes the peer sent after the last message", drained);
   }
   else if (ec != errc::not_connected) // the peer may be gone already
      mlogw("shutdown: {}", ec.message());
   io::close(stream_);
   stream_closed_ = true;
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
void H1Session<Stream>::destroy() noexcept
{
   if (stream_closed_)
      return;

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

/// Whether the comma-separated \p list has \p token in it, in any case.
static bool has_token(std::string_view list, std::string_view token)
{
   auto tokens = boost::urls::grammar::parse(list, http::list_rule(http::token_rule, 1));
   return tokens && std::ranges::any_of(*tokens, [&](auto item) {
             return boost::urls::grammar::ci_is_equal(item, token);
          });
}

/**
 * Decodes \p text from base64url without padding (RFC 4648, section 5), what 'HTTP2-Settings'
 * holds (RFC 7540, section 3.2.1). Fails on anything else.
 */
static std::optional<std::string> base64url_decode(std::string_view text)
{
   const auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z')
         return c - 'A';
      if (c >= 'a' && c <= 'z')
         return c - 'a' + 26;
      if (c >= '0' && c <= '9')
         return c - '0' + 52;
      if (c == '-')
         return 62;
      if (c == '_')
         return 63;
      return -1;
   };

   if (text.size() % 4 == 1)
      return std::nullopt;

   std::string result;
   result.reserve(text.size() * 3 / 4);
   unsigned bits = 0;
   int count = 0;
   for (char c : text)
   {
      const int v = value(c);
      if (v < 0)
         return std::nullopt;
      bits = (bits << 6) | static_cast<unsigned>(v);
      if (++count == 4)
      {
         result.push_back(static_cast<char>(bits >> 16));
         result.push_back(static_cast<char>(bits >> 8));
         result.push_back(static_cast<char>(bits));
         bits = 0;
         count = 0;
      }
   }
   if (count == 2)
      result.push_back(static_cast<char>(bits >> 4));
   else if (count == 3)
   {
      result.push_back(static_cast<char>(bits >> 10));
      result.push_back(static_cast<char>(bits >> 2));
   }
   return result;
}

/**
 * Returns what the HTTP/2 session needs to continue \p request as stream 1, if the request asks
 * for an upgrade to h2c (RFC 7540, section 3.2) and it can be granted. Otherwise, the upgrade is
 * ignored and the request is served as HTTP/1.1, which is always a valid response to it.
 *
 * Only requests that are complete after their header are upgraded: A request body would have to
 * be read in HTTP/1.1 first, before switching protocols. Cleartext only, as h2 over TLS is
 * negotiated by ALPN instead.
 */
static std::optional<h2::Upgrade> h2c_upgrade(std::string_view log_prefix,
                                              const http::static_request& request,
                                              const boost::urls::url& url, bool complete)
{
   //
   // nghttp2 version 2 dropped the h2c upgrade along with RFC 7540 -- there is no counterpart of
   // nghttp2_session_upgrade2() to continue the request as stream 1 with. Declining the upgrade is
   // always a valid answer to it, so a tree built with H2_LIBRARY=nghttp2v2 serves the request as
   // HTTP/1.1 instead.
   //
   if constexpr (ANYHTTP_H2_V2)
      return std::nullopt;

   if (!has_token(request.value_or(field::upgrade, ""), "h2c") || url.scheme() != "http")
      return std::nullopt;

   const auto connection = request.value_or(field::connection, "");
   if (!has_token(connection, "upgrade") || !has_token(connection, "http2-settings"))
   {
      logw("[{}] upgrade: ignoring h2c upgrade, 'Connection' misses 'Upgrade' or 'HTTP2-Settings'",
           log_prefix);
      return std::nullopt;
   }

   // exactly one HTTP2-Settings header, containing base64url without padding
   if (request.count(field::http2_settings) != 1)
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

   // a SETTINGS payload is a sequence of 6-byte entries
   auto settings = base64url_decode(request.at(field::http2_settings));
   if (!settings || settings->size() % 6 != 0)
   {
      logw("[{}] upgrade: ignoring h2c upgrade, invalid 'HTTP2-Settings' header", log_prefix);
      return std::nullopt;
   }

   h2::Upgrade upgrade;
   upgrade.settings = std::move(*settings);
   upgrade.method = request.method_text();
   upgrade.url = url;

   //
   // HTTP/2 has no connection-specific header fields (RFC 9113, section 8.2.2), and the upgrade
   // ones are used up by now.
   //
   static constexpr std::array connection_specific{field::connection, field::proxy_connection,
                                                   field::keep_alive, field::transfer_encoding,
                                                   field::upgrade,    field::http2_settings};
   for (const auto& item : request)
      if (!item.id || !std::ranges::contains(connection_specific, *item.id))
         upgrade.fields.append(item.name, item.value);
   return upgrade;
}

/**
 * This function waits for headers of an incoming, new request and passes control to a registered
 * handler. After the request has been completed, and if the connection can be kept open, it starts
 * waiting again.
 *
 * Requests are served one at a time, also when the client pipelines them: those that follow wait in
 * the parser, and the next one is read only after the handler of the previous one has returned, so
 * the responses go out in order. A response the handler has not completed by then is detached, and
 * writing to it fails with \c connection_aborted.
 *
 * Requests and responses that outlive the session are detached as well, see ~H1Session().
 */
template <typename Stream>
Task<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   pending_ = std::move(buffer);

   mlogd("do_server_session, {} bytes in buffer", pending_.size());

   size_t requestCounter = 0;
   while (!closed_)
   {
      detach_readers(); // the previous request, if still around, is done with the stream

      //
      // What is left of the previous message can not be told apart from whatever follows it on
      // the connection, and a response that was begun and not ended leaves the client waiting for
      // the rest of it: either way, nothing more can go over this connection.
      //
      if (requestCounter && !parser_.is_complete())
      {
         mlogw("previous request not read completely, closing connection");
         break;
      }
      if (!serializer_.is_done())
      {
         mlogw("previous response not complete, closing connection");
         break;
      }

      parser_.start();
      auto reader = std::make_unique<RequestReader<Stream>>(*this, parser_);

      logd("");
      mlogd("waiting for request ({} bytes pending)", pending_.size());
      auto [ec] = co_await h1::read_header(stream_, pending_, parser_);
      if (!ec)
         mlogd("async_read_header: ok");
      else if (ec == errors::end_of_stream)
         mlogd("async_read_header: end of stream");
      else
         mlogw("async_read_header: \x1b[1;31m{}\x1b[0m", ec.message());

      //
      // The rest of the request can not be told apart from whatever follows it on the connection,
      // so there is nothing left to do after telling the client why.
      //
      if (ec == errors::header_limit)
      {
         http::response res(http::status::request_header_fields_too_large);
         res.set(field::server, "anyhttp");
         res.set(field::connection, "close");
         res.set_content_length(0);
         if (auto [ec] = co_await h1::write_header(stream_, serializer_, res, false); ec)
            mlogw("writing 431 response: {}", ec.message());
      }
      if (ec)
         break;
      requestCounter++;

      auto& request = parser_.get();
      const bool need_eof = !request.keep_alive();
      reader->method_ = request.method_text();
      reader->on_header();

      if (auto url = boost::urls::parse_uri_reference(request.target()); url.has_value())
         reader->url_ = url.value();
      else
         mlogw("{} {}: invalid target: {}", request.method_text(), request.target(),
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

      const auto host = request.value_or(field::host, "");
      try
      {
         reader->url_.set_encoded_authority(host);
      }
      catch (std::exception& ex)
      {
         mlogw("ignoring invalid host header: {}", host);
      }

      mlogd("{} {} (need_eof={})", request.method_text(), reader->url_.buffer(), need_eof);
      for (const auto& header : request)
         mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(header.name), truncated(header.value));

      //
      // Upgrade to h2c, if requested: Answer with "101 Switching Protocols" and hand over the
      // stream to an HTTP/2 session, which continues this request as stream 1. Anything that
      // follows the request (the client preface) is already HTTP/2: what the parser holds of it,
      // and what it has not taken yet.
      //
      if (auto upgrade = h2c_upgrade(log_prefix(), request, reader->url_, parser_.is_complete()))
      {
         Buffer data;
         const auto leftover = parser_.release_buffered_data();
         buffer_copy(data.prepare(leftover.size()), const_buffer(leftover.data(), leftover.size()));
         data.commit(leftover.size());
         buffer_copy(data.prepare(pending_.size()), pending_.data());
         data.commit(pending_.size());
         pending_.clear();
         reader.reset();

         http::response res(http::status::switching_protocols);
         res.set(field::connection, "Upgrade");
         res.set(field::upgrade, "h2c");
         if (auto [ec] = co_await h1::write_header(stream_, serializer_, res, false); ec)
         {
            mlogw("upgrade: writing 101 response: {}", ec.message());
            break;
         }

         mlogi("upgrading to h2c, {} bytes in buffer", data.size());
         // Stream is whatever this session runs on, but never a TLS one: h2c_upgrade() takes
         // cleartext requests only, as h2 over TLS is negotiated by ALPN instead.
         upgraded_ = h2::make_server_session(server(), super::get_executor(), std::move(stream_),
                                             std::move(*upgrade));
         co_await upgraded_->do_session(std::move(data));
         mlogi("h2c session done, served {} requests before upgrade", requestCounter - 1);
         co_return;
      }

      //
      // Prepare response.
      //
      detach_writers(); // likewise for the previous response
      auto writer = std::make_shared<ResponseWriter<Stream>>(*this);

      http::response& response = writer->message;
      response.set(field::server, "anyhttp");

      //
      // Point the client at our HTTP/3 endpoint, see server::Config::alt_svc_max_age. Set before
      // the handler runs, so one that wants to say something else about alternative services can
      // simply pass its own field: submitting a response replaces the fields it names.
      //
      if (const auto& alt_svc = server().alt_svc(); !alt_svc.empty())
         response.set(field::alt_svc, alt_svc);

      //
      // A client that asked for the connection to end -- or one speaking HTTP/1.0, which has no
      // persistent connections unless it asks for one -- gets one last response. A request handler
      // is not allowed to sidestep this by keeping the connection alive.
      //
      if (need_eof)
         response.set_keep_alive(false);

      //
      // Call user-provided request handler.
      //
      // Unlike HTTP2, the request handler is not co_spawn()ed as a separate thread of execution,
      // because HTTP/1.1 does not do multiplexing.
      //
      // TODO: Pipelined requests could be served concurrently, as long as their responses still go
      //       out in order. That needs reading requests and writing responses to be decoupled: a
      //       queue of incoming requests, and one of outgoing responses that are written in turn
      //       while later ones are already being produced.
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

      mlogd("request handler finished ({} bytes pending)", pending_.size());

      //
      // Honor 'Connection: close', even if the handler has not responded at all. A response that
      // says "close" ends the session, too, see ResponseWriter::submit().
      //
      if (need_eof)
      {
         mlogd("request needs EOF, closing connection");
         break;
      }
   }

   mlogi("closing stream, served {} requests", requestCounter);

   co_await super::end_connection();

   mlogd("session done");
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
Task<void> ClientSession<Stream>::do_session(Buffer&& buffer)
{
   pending_ = std::move(buffer);

   mlogd("do_client_session, {} bytes in buffer", pending_.size());

   // Set the low-level TCP stream timeout. This is relevant for some testcases...
   // stream_.expires_after(5s);

   //
   // Nothing to do here: in HTTP/1.1, each request writes itself and each response reads itself,
   // and the session only keeps them in order (see ClientSession). Pipelining needs no queue
   // either, as a request is written as a whole before the next one may be submitted, and
   // responses are read in the order of their requests. Requests and responses that outlive the
   // session are detached, see ~H1Session().
   //

   //
   // TODO: There should be something that keeps the session alive. We are just waiting for
   //       the client to make a request here right now...
   //
   co_return;
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
IoTask<> ClientSession<Stream>::shutdown()
{
   if (std::exchange(shutting_down_, true))
      co_return std::tuple{error_code{}};

   const auto ops = stream_ops_;
   const bool intact = !send_failed_ && ops->writes == 0;
   mlogd("shutdown: {} operations in progress, {} writes", ops->pending, ops->writes);

   //
   // What is left of the requests and responses fails like with a session that is gone, and nothing
   // new can get to the stream any more. What is on it right now has to be off it before TLS can
   // shut down, which must not overlap a read.
   //
   send_failed_ = receive_failed_ = true;
   sending_ = nullptr;
   detach_writers();
   detach_readers();

   io::cancel(stream_);
   if (ops->pending)
   {
      ops->idle.clear();
      co_await ops->idle.wait();
   }

   co_await super::end_connection(intact);
   co_return std::tuple{intact ? error_code{} : errors::connection_aborted};
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

/// The I/O is done in a coroutine, see H1Reader.
template <typename Stream>
void ClientSession<Stream>::async_submit(SubmitHandler&& handler, std::string_view method,
                                         boost::urls::url target, const Fields& headers)
{
   launch(super::get_executor(), submit(std::string(method), std::move(target), headers),
          std::move(handler));
}

template <typename Stream>
IoTask<client::Request> ClientSession<Stream>::submit(std::string method, boost::urls::url target,
                                                      Fields headers)
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

   auto writer = std::make_unique<RequestWriter<Stream>>(*this);
   auto& request = writer->message;

   //
   // FIXME: https://datatracker.ietf.org/doc/html/rfc7230#section-5.3
   //        Usually, target should be in "origin form", but we need to support other forms, too.
   //
   // The request checks what it is given: a method or target with whitespace in it, or a header
   // field that is not one, is not something to put on the wire.
   //
   try
   {
      request.set_start_line(method, target.encoded_target());
      request.set(field::user_agent, "anyhttp");
      add_fields(request, headers);
      if (!request.exists(field::host))
         request.set(field::host, target.encoded_host_and_port());
      if (!has_content_length(request))
         request.set_chunked(true);
   }
   catch (const std::exception& ex)
   {
      mlogw("async_submit: invalid request: {}", ex.what());
      co_return std::tuple{make_error_code(errc::invalid_argument), client::Request{}};
   }

   mlogd("{} {}", request.method_text(), target.buffer());
   writer->log_fields();

   writer->sequence = requests_sent_++;
   sending_ = writer.get();

   std::tie(ec) = co_await writer->write_head(writer->has_body());
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
   // The connection is in the middle of this request, and nothing else can be sent any more. Nor
   // is the rest of this one coming: the end of the stream is what tells the server, which would
   // otherwise wait for it, and the client for the response, forever.
   //
   if (sending_ == &request)
   {
      request_failed(request);
      auto ec = io::shutdown(stream_, io::Shutdown::send);
      logwd(ec && ec != errc::not_connected, "[{}] request #{} released: shutdown: {}",
            log_prefix(), request.sequence, ec.message());
   }

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
// translation unit, so that the generic server and client stay free of Boost.HTTP's parser and
// serializer.
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

} // namespace anyhttp::h1
