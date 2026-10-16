#pragma once

#include "anyhttp/common.hpp"
#include "anyhttp/quic/stream.hpp"
#include "anyhttp/reader_impl.hpp"
#include "anyhttp/writer_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>
#include <boost/url/urls.hpp>

#include <nghttp3/nghttp3.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anyhttp::http3
{

class Http3Application;

// =================================================================================================

//
// One HTTP/3 request/response exchange: a QUIC stream (which has the body, both ways) with the
// header sections on top, and nghttp3 between it and ngtcp2. Both roles use this same class; what
// differs is only which half of the exchange travels in which direction -- the server reads a
// request and writes a response, the client writes a request and reads a response -- so
// everything below is phrased as the *incoming* and the *outgoing* message, and the handful of
// genuinely role-specific steps (which pseudo-headers to parse, what to do once the incoming
// headers are complete, how the outgoing headers are submitted) are virtual hooks implemented by
// Http3ServerStream / Http3ClientStream.
//
class Http3Stream : public quic::QuicStream
{
public:
   Http3Stream(Http3Application& application, int64_t id, quic::WriteMode write_mode);
   ~Http3Stream() override;

   Http3Application& application;

   //
   // Incoming message (request on the server, response on the client), populated by the nghttp3
   // header callbacks. Only one of method/status_code is ever meaningful, depending on the role.
   //
   std::string method;
   boost::urls::url url;
   unsigned int status_code = 0;
   std::optional<size_t> content_length;
   Fields fields;

   //
   // The header block as it arrived, buffered so that end_headers can log it in one go, below the
   // request/status line, instead of one stray line per header as they come in. Only filled when
   // debug logging is on, and dropped again as soon as it has been logged.
   //
   std::vector<std::pair<std::string, std::string>> received_headers;
   bool headers_received = false;

   /// Size of the received header section so far, see Config::max_header_size.
   size_t header_size = 0;
   bool header_limit_exceeded = false;

   //
   // Outgoing message: the response on the server (set by the user through Http3Writer), the
   // request on the client (set once by async_submit(), before this stream even exists as far as
   // the user is concerned).
   //
   unsigned int response_status = 0;
   Fields response_fields;
   std::optional<size_t> response_content_length;
   std::string response_content_length_str; // storage backing a nghttp3_nv
   bool headers_submitted = false;

   //
   // Outgoing body, as nghttp3 sees it. It takes each piece of data once and keeps it, and would
   // take a repeated offer as *additional*, distinct stream bytes -- duplicating the body on the
   // wire -- so data_reader() offers from tx_offered on, and a repeat call gets
   // NGHTTP3_ERR_WOULDBLOCK. What it reports back is counted in body bytes for acknowledgements,
   // but including its framing for what went into packets, see on_write_offered().
   //
   nghttp3_ssize data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags);
   void on_write_acked(size_t n); // nghttp3 acked_stream_data
   void on_write_offered(size_t n); // bytes ngtcp2 committed to a packet

   //
   // Headers.
   //
   void on_header(std::string_view name, std::string_view value);
   void on_end_headers();

   /// Hands an assembled header block to nghttp3, as a request (client) or response (server).
   bool submit_headers(std::span<const nghttp3_nv> nva, bool is_request);

   void delete_writer() override;

protected:
   //
   // QuicStream
   //
   void on_writable() override;
   uint64_t reset_error_code() const noexcept override { return NGHTTP3_H3_REQUEST_CANCELLED; }
   uint64_t stop_sending_error_code() const noexcept override { return NGHTTP3_H3_NO_ERROR; }

   //
   // Role-specific pieces. Everything else above is shared verbatim between server and client.
   //
   /// Parse a `:`-prefixed pseudo-header of the incoming message.
   virtual void on_pseudo_header(std::string_view name, std::string_view value) = 0;
   /// The incoming header block is complete: dispatch the request (server) / response (client).
   virtual void on_headers_complete() = 0;

public:
   /// Submit the outgoing response headers. A no-op on the client, whose request headers went out
   /// with the stream itself, see Http3ClientApplication::async_submit().
   virtual void submit_response(unsigned int status_code, const Fields& fields) = 0;
};

// =================================================================================================
// Http3Reader / Http3Writer: adapters plugging an Http3Stream into the anyhttp Reader/Writer
// interfaces. The same pair serves both roles -- server::Request/server::Response and
// client::Response/client::Request are the same two halves seen from the other end.
// =================================================================================================

template <typename Interface>
class Http3Reader : public Interface
{
public:
   explicit Http3Reader(Http3Stream& s) : stream(&s), executor(s.get_executor())
   {
      s.reader = this;
   }
   ~Http3Reader() override
   {
      if (stream)
      {
         stream->reader = nullptr;
         stream->delete_reader();
      }
   }

   Executor get_executor() const noexcept override { return executor; }

   std::optional<size_t> content_length() const noexcept override
   {
      return stream ? stream->content_length : std::nullopt;
   }

   const Fields& fields() const override { return stream ? stream->fields : detached_fields; }

   void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) override
   {
      //
      // An empty buffer is not a request to read anything: complete right away, without looking
      // at whether the body has ended or the stream is even still there -- as ASIO does for a
      // zero-length read.
      //
      if (asio::buffer_size(buffer) == 0)
      {
         complete_immediately(std::move(handler), executor, error_code{}, size_t{0});
         return;
      }

      //
      // The stream is gone; detach() latched how the body stood at that point, so a cleanly
      // finished body keeps reporting eof (as the Reader contract requires) and a truncated one
      // keeps reporting partial_message.
      //
      if (!stream)
      {
         complete_immediately(std::move(handler), executor, detached_ec, size_t{0});
         return;
      }

      on_cancel(handler, [this] {
         if (stream && stream->read_handler)
            complete_later(std::move(stream->read_handler), stream->get_executor(),
                           errors::canceled, size_t{0});
      });

      assert(!stream->read_handler);
      stream->read_handler = std::move(handler);
      stream->read_handler_buffer = buffer;
      stream->call_read_handler();
   }

   void detach() override
   {
      //
      // The stream is going away first (session teardown outliving this exchange). Remember how
      // the body stood, so that reads issued from now on keep answering per the Reader contract.
      //
      assert(stream);
      detached_ec = stream->reading_finished() ? errors::eof : errors::partial_message;
      detached_log_prefix = stream->log_prefix();
      detached_fields = std::move(stream->fields); // the stream is being destroyed
      stream = nullptr;
   }

   std::string log_prefix() const override
   {
      return stream ? stream->log_prefix() : detached_log_prefix;
   }

   Http3Stream* stream;
   Executor executor; // kept as a copy so a detached reader can still complete
   std::string detached_log_prefix; // latched by detach()

   /// What a read past detach() reports: eof for a body read to its clean end, else truncation.
   error_code detached_ec{errors::partial_message};
   Fields detached_fields; // moved out of the stream by detach(), so fields() outlives it
};

// -------------------------------------------------------------------------------------------------

template <typename Base>
class Http3Writer : public Base
{
public:
   explicit Http3Writer(Http3Stream& s) : stream(&s), executor(s.get_executor())
   {
      s.writer = this;
   }
   ~Http3Writer() override
   {
      if (stream)
      {
         stream->writer = nullptr;
         stream->delete_writer();
      }
   }

   Executor get_executor() const noexcept override { return executor; }

   void content_length(std::optional<size_t> len) override
   {
      assert(stream);
      stream->response_content_length = len;
   }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) override
   {
      if (stream)
      {
         // everything -- including a write against a stream ngtcp2 has already torn down -- is
         // start_write()'s to decide, so that ending a body twice and writing past its end are
         // answered the same way whatever became of the stream since
         stream->start_write(std::move(handler), buffer, eof);
         return;
      }

      //
      // The stream itself is gone, but the entry ladder of the Writer contract still applies,
      // answered from the state detach() latched: an empty non-EOF write stays a free no-op, a
      // body that was cleanly ended keeps answering as such -- bare re-end idempotent, data
      // broken_pipe -- and only a stream that vanished mid-body is a connection error.
      //
      const bool empty = asio::buffer_size(buffer) == 0;
      error_code ec;
      if (empty && !eof)
         ec = {};
      else if (detached_body_ended)
         ec = empty ? error_code{} : make_error_code(errc::broken_pipe);
      else
         ec = make_error_code(errc::connection_reset);
      complete_immediately(std::move(handler), executor, ec);
   }

   void async_submit(StatusHandler&& handler, unsigned int status_code, const Fields& fields)
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(make_error_code(errc::connection_reset));
         return;
      }
      stream->submit_response(status_code, fields);
      std::move(handler)(error_code{});
   }

   void detach() override
   {
      // remember whether the body was cleanly ended -- intent accepted *and* the FIN handed to
      // nghttp3 -- so writes issued after this still answer per the Writer contract
      assert(stream);
      detached_body_ended = stream->eof_submitted && stream->fin_offered;
      detached_log_prefix = stream->log_prefix();
      stream = nullptr;
   }

   std::string log_prefix() const override
   {
      return stream ? stream->log_prefix() : detached_log_prefix;
   }

   Http3Stream* stream;
   Executor executor; // kept as a copy so a detached writer can still complete
   std::string detached_log_prefix; // latched by detach()
   bool detached_body_ended = false; // latched by detach(), see there
};

// =================================================================================================

} // namespace anyhttp::http3
