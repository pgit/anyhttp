#pragma once

#include "anyhttp/common.hpp"

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>
#include <boost/url/urls.hpp>

#include <nghttp3/nghttp3.h>

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anyhttp::http3
{

class Http3Session;

// =================================================================================================

//
// How a stream hands the caller's async_write() buffer down to nghttp3.
//
enum class WriteMode
{
   //
   // Point the nghttp3_vec straight into the caller's buffer: the body is never copied on its way
   // to the nghttp3/ngtcp2 boundary, however large it is -- the mmap()ed file of serve_file()
   // travels from the page cache into QUIC packets without an intermediate byte. What that costs
   // is *when* the write completes: ngtcp2 keeps pointing into that memory for as long as the
   // bytes may still have to be retransmitted, so the handler -- which releases the buffer -- can
   // only run once they are acknowledged (nghttp3's acked_stream_data callback). Cancelling a
   // write with unacknowledged bytes therefore has to reset the stream, there being no way to
   // un-offer memory ngtcp2 may still read from.
   //
   ZeroCopy,

   //
   // Copy through a bounded, stream-owned staging buffer (write_chunk, <= kWriteChunkSize),
   // refilled as nghttp3 drains it. That costs a copy but completes a write as soon as the bytes
   // are handed over, and makes cancellation instantaneous: nothing points into the caller's
   // buffer, so the un-copied remainder is simply abandoned and the *stream survives* -- which is
   // what lets a cancelled write be followed by another one on the same stream.
   //
   Staged
};

// =================================================================================================

//
// One HTTP/3 request/response exchange: a bidirectional stream that can be read from and written
// to. Both roles use this same class; what differs is only which half of the exchange travels in
// which direction -- the server reads a request and writes a response, the client writes a request
// and reads a response -- so everything below is phrased as the *incoming* and the *outgoing*
// message, and the handful of genuinely role-specific steps (which pseudo-headers to parse, what
// to do once the incoming headers are complete, how the outgoing headers are submitted) are
// virtual hooks implemented by Http3ServerStream / Http3ClientStream.
//
class Http3Stream : public std::enable_shared_from_this<Http3Stream>
{
public:
   Http3Stream(Http3Session& session, int64_t id, WriteMode write_mode);
   virtual ~Http3Stream();

   int64_t id;
   Http3Session& session;
   std::string log_prefix;

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
   // Incoming body. What nghttp3 delivers is parked here only if no reader was waiting for it.
   //
   std::deque<std::vector<uint8_t>> pending_read;
   asio::const_buffer read_head; // view of pending_read.front() not yet delivered
   asio::const_buffer incoming; // chunk on_data_chunk() is delivering, not yet taken
   bool eof_received = false;
   ReadSomeHandler read_handler;
   asio::mutable_buffer read_handler_buffer;
   bool call_read_handler_active = false; // re-entrancy guard, see call_read_handler()

   //
   // Outgoing body. Only one async_write() may be active at a time -- callers must wait for its
   // handler before issuing another (same contract as e.g. Beast) -- so this is flat per-stream
   // state rather than a queue of pending writes.
   //
   // write_source is the caller's buffer, referenced, not copied, the way asio::async_write
   // generally requires: it must stay valid until write_handler fires. write_offered tracks how
   // much of it has been handed to nghttp3, which may ask again before any of it goes out and
   // would take a repeated offer as *additional*, distinct stream bytes -- duplicating the body on
   // the wire -- so a repeat call gets NGHTTP3_ERR_WOULDBLOCK instead.
   //
   // What completes the write depends on write_mode: bytes acknowledged (write_acked) in
   // ZeroCopy, bytes copied out and handed over (write_source_copied / write_confirmed) in
   // Staged. See WriteMode.
   //
   const WriteMode write_mode;
   bool write_active = false;
   asio::const_buffer write_source;
   size_t write_offered = 0;
   size_t write_acked = 0; // ZeroCopy: reported by nghttp3's acked_stream_data
   size_t write_source_copied = 0; // Staged: how much of write_source went into a chunk
   std::vector<uint8_t> write_chunk; // Staged: the staging buffer itself
   size_t write_confirmed = 0; // Staged: how much of write_chunk ngtcp2 has taken
   std::vector<std::vector<uint8_t>> in_flight_writes; // Staged: retired chunks, kept alive for
                                                       // the stream's lifetime because ngtcp2 may
                                                       // still retransmit from them
   //
   // Whether the active write ends the body. data_reader() then hands nghttp3
   // NGHTTP3_DATA_FLAG_EOF along with the write's last bytes -- one QUIC STREAM frame carrying
   // both the tail of the body and the FIN -- rather than needing a write of its own for it.
   //
   bool write_is_eof = false;
   WriteHandler write_handler;
   uint64_t write_token = 0;
   uint64_t next_write_token = 1;
   bool eof_submitted = false; // user ended the body via async_write_eof()
   bool fin_offered = false; // ... and data_reader() has handed the FIN flag to nghttp3; between
                             // the two lies a cancelled async_write_eof(), which rolls
                             // eof_submitted back when the FIN is still owed

   //
   // Lifecycle.
   //
   impl::Reader* reader = nullptr; // the Http3Reader, while attached
   impl::Writer* writer = nullptr; // the Http3Writer, while attached
   bool closed = false;

   asio::any_io_executor get_executor() const noexcept;
   const std::string& logPrefix() const noexcept { return log_prefix; }

   //
   // Data flow into user land (incoming body).
   //
   void on_data_chunk(const uint8_t* data, size_t len);
   void on_eof();
   void call_read_handler();

   /// True once the peer ended the body and every byte of it has been delivered to the reader.
   bool reading_finished() const noexcept
   {
      return eof_received && read_head.size() == 0 && incoming.size() == 0;
   }

   //
   // Data flow from user land back to nghttp3 (outgoing body).
   //
   void start_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof);
   nghttp3_ssize data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags);
   void on_write_acked(size_t n); // ZeroCopy: nghttp3 acked_stream_data
   void on_write_offered(size_t n); // Staged: bytes ngtcp2 committed to a packet

   //
   // Headers.
   //
   void on_header(std::string_view name, std::string_view value);
   void on_end_headers();

   /// Hands an assembled header block to nghttp3, as a request (client) or response (server).
   bool submit_headers(std::span<const nghttp3_nv> nva, bool is_request);

   //
   // Called when the stream dies before its exchange completed, and from either the reader's or
   // the writer's destructor.
   //
   void fail(boost::system::error_code ec);
   void delete_reader();
   void delete_writer();
   void maybe_close();

protected:
   //
   // Role-specific pieces. Everything else above is shared verbatim between server and client.
   //
   /// Parse a `:`-prefixed pseudo-header of the incoming message.
   virtual void on_pseudo_header(std::string_view name, std::string_view value) = 0;
   /// The incoming header block is complete: dispatch the request (server) / response (client).
   virtual void on_headers_complete() = 0;
   /// The stream failed or closed early; fail whatever else the role has pending.
   virtual void on_failed(boost::system::error_code ec) { (void)ec; }

public:
   /// Submit the outgoing response headers. A no-op on the client, whose request headers went out
   /// with the stream itself, see Http3ClientSession::async_submit().
   virtual void submit_response(unsigned int status_code, const Fields& fields) = 0;

private:
   void bind_write_cancellation(WriteHandler& handler, uint64_t token); // arms cancellation
   void finish_active_write(); // completes the active write and releases the caller's buffer
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

   asio::any_io_executor get_executor() const noexcept override { return executor; }

   std::optional<size_t> content_length() const noexcept override
   {
      return stream ? stream->content_length : std::nullopt;
   }

   /// Only meaningful for a client::Response; a server::Request has no status, and reports 0.
   unsigned int status_code() const noexcept override { return stream ? stream->status_code : 0; }

   boost::url_view url() const override
   {
      assert(stream);
      return stream->url;
   }

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

      auto cs = asio::get_associated_cancellation_slot(handler);
      if (cs.is_connected() && !cs.has_handler())
      {
         cs.assign([this](asio::cancellation_type_t)
         {
            if (stream && stream->read_handler)
            {
               asio::post(stream->get_executor(),
                          [handler = std::move(stream->read_handler)]() mutable
               {
                  std::move(handler)(
                     boost::system::errc::make_error_code(boost::system::errc::operation_canceled),
                     0);
               });
            }
         });
      }

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
      detached_ec = stream->reading_finished()
                       ? error_code{asio::error::eof}
                       : error_code{boost::beast::http::error::partial_message};
      stream = nullptr;
   }

   Http3Stream* stream;
   asio::any_io_executor executor; // kept as a copy so a detached reader can still complete

   /// What a read past detach() reports: eof for a body read to its clean end, else truncation.
   error_code detached_ec{boost::beast::http::error::partial_message};
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

   asio::any_io_executor get_executor() const noexcept override { return executor; }

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
         ec = empty ? error_code{}
                    : boost::system::errc::make_error_code(boost::system::errc::broken_pipe);
      else
         ec = boost::system::errc::make_error_code(boost::system::errc::connection_reset);
      complete_immediately(std::move(handler), executor, ec);
   }

   void async_submit(StatusHandler&& handler, unsigned int status_code, const Fields& fields)
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(
            boost::system::errc::make_error_code(boost::system::errc::connection_reset));
         return;
      }
      stream->submit_response(status_code, fields);
      std::move(handler)(boost::system::error_code{});
   }

   void detach() override
   {
      // remember whether the body was cleanly ended -- intent accepted *and* the FIN handed to
      // nghttp3 -- so writes issued after this still answer per the Writer contract
      assert(stream);
      detached_body_ended = stream->eof_submitted && stream->fin_offered;
      stream = nullptr;
   }

   Http3Stream* stream;
   asio::any_io_executor executor; // kept as a copy so a detached writer can still complete
   bool detached_body_ended = false; // latched by detach(), see there
};

// =================================================================================================

} // namespace anyhttp::http3
