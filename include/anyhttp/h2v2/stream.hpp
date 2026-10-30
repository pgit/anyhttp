#pragma once

//
// The HTTP/2 stream, nghttp2 version 2. The counterpart of h2/stream.hpp, with the same
// definitions in the same order, so that the two can be read side by side -- see H2_LIBRARY.
//

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/reader_impl.hpp"
#include "anyhttp/writer_impl.hpp"

#include <nghttp2v2/nghttp2.h>

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/system_category.hpp>

#include <boost/url/url.hpp>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace anyhttp::nghttp2
{

// =================================================================================================

//
// How a stream hands the caller's async_write() buffer down to nghttp2. The counterpart of
// quic::WriteMode, for the same reason: version 2 takes a body by reference, so the two have the
// same problem to solve, and solve it the same way. Version 1 copied and needs none of this.
//
enum class WriteMode
{
   //
   // Point straight into the caller's buffer: the body is never copied on its way into a DATA
   // frame, however large it is. What that costs is *when* the write completes -- nghttp2 keeps
   // reading from that memory until it has framed all of it, which the peer's flow control can
   // postpone indefinitely, so the handler, which releases the buffer, can only run once the bytes
   // have been framed. Cancelling a write with bytes still outstanding therefore has to reset the
   // stream, there being no way to un-offer memory nghttp2 may still read from.
   //
   ZeroCopy,

   //
   // Copy through a bounded, stream-owned chunk (write_chunk_size), the next one carved off the
   // caller's buffer once the one before has been framed. That costs a copy -- the one version 1
   // makes anyway -- but completes a write as soon as the bytes are handed over, and makes
   // cancellation instantaneous: nothing points into the caller's buffer, so the remainder is
   // simply abandoned and the *stream survives*, which is what lets a cancelled async_write_eof()
   // end the body for real afterwards.
   //
   Staged
};

// -------------------------------------------------------------------------------------------------

class NGHttp2Stream;

template <typename Interface>
class NGHttp2Reader : public Interface
{
public:
   explicit NGHttp2Reader(NGHttp2Stream& stream);
   ~NGHttp2Reader() override;

   Executor get_executor() const noexcept override;
   std::optional<size_t> content_length() const noexcept override;
   void async_read_some(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler) override;
   void detach() override;
   std::string log_prefix() const override;

   const Fields& fields() const override;

   NGHttp2Stream* stream;
   Executor executor; // kept as a copy so a detached reader can still complete
   std::string detached_log_prefix; // latched by detach()

   /// What a read past detach() reports, latched by detach(): the stream may be gone, but a body
   /// that was read to its clean end keeps ending in \c eof, a truncated one in partial_message.
   error_code detached_ec{errors::partial_message};
   Fields detached_fields; // moved out of the stream by detach(), so fields() outlives it
};

// -------------------------------------------------------------------------------------------------

template <typename Base>
class NGHttp2Writer : public Base
{
public:
   explicit NGHttp2Writer(NGHttp2Stream& stream);
   ~NGHttp2Writer() override;

   Executor get_executor() const noexcept override;
   void content_length(std::optional<size_t> content_length) override;
   void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) override;
   void detach() override;
   std::string log_prefix() const override;

   void async_submit(StatusHandler&& handler, unsigned int status_code, const Fields& headers);
   void async_get_response(client::Request::GetResponseHandler&& handler);

   NGHttp2Stream* stream;
   Executor executor; // kept as a copy so a detached writer can still complete
   std::string detached_log_prefix; // latched by detach()
   bool detached_eof_submitted = false; // latched by detach(): the body was cleanly ended
   std::optional<size_t> content_length_;
};

// =================================================================================================

class NGHttp2Session;
class NGHttp2Stream : public std::enable_shared_from_this<NGHttp2Stream>
{
public:
   size_t bytesRead = 0;
   size_t bytesWritten = 0;

   // ----------------------------------------------------------------------------------------------

   /**
    * True after we have received an EOF flag from the peer. After this, no more buffers will be
    * added. But there might be still some buffers left to be delivered to the user.
    */
   bool eof_received = false;

   /**
    * Buffers received from peer, pending delivery to the user. The receiver tries to avoid
    * buffering if possible, but if there is currently no user-provided read handler to deliver
    * the data to, we have to store it -- decreasing the stream's receive window in the process.
    *
    * As long as \c pending_read_buffers_ is non-empty, \c read_buffer_ is viewing the part of
    * the first pending read buffer that has not been delivered, yet.
    */
   asio::const_buffer read_buffer_;
   using Buffer = std::vector<uint8_t>;
   std::deque<Buffer> pending_read_buffers_;

   Buffer make_buffer(asio::const_buffer buffer)
   {
      return {static_cast<const uint8_t*>(buffer.data()),
              static_cast<const uint8_t*>(buffer.data()) + asio::buffer_size(buffer)};
   }

   static bool is_empty(asio::const_buffer buffer) { return asio::buffer_size(buffer) == 0; }

   /**
    * Returns true if all data has been read by the user.
    * This is true if there was an EOF flag and all buffers have been consumed.
    */
   bool reading_finished() const { return !reader || eof_received && is_empty(read_buffer_); }

   /**
    * Returns the number of bytes left to read. This is the remaining part of the first buffer
    * and the sum of all following buffers.
    */
   size_t read_buffers_size() const;

   // ----------------------------------------------------------------------------------------------

   //
   // async_write()
   //
   // Version 2 takes the body by reference: data_reader() hands over an nghttp2_vec, and nghttp2
   // keeps reading from that memory until it has framed every byte of it -- across as many
   // nghttp2_conn_write() calls as flow control makes it wait, because the vec lives in the
   // stream's writer and is only replaced once it has been consumed. Version 1's producer callback
   // copied into a buffer of nghttp2's and was done with the caller's right away.
   //
   // \c offered_ counts how much of \c write_buffer nghttp2 has been given, \c written_ how much of
   // it has made it into a frame, which on_write_offset() reports. What the two are for depends on
   // \c write_mode: in ZeroCopy the vec points into \c write_buffer itself, so the handler may only
   // run once \c written_ has caught up; in Staged it points into \c write_chunk_, a copy, so the
   // handler runs as soon as the last of the buffer has been copied out of it.
   //
   static constexpr size_t write_chunk_size = 16 * 1024; // one DATA frame, one send loop pass
   const WriteMode write_mode;
   asio::const_buffer write_buffer; // undefined unless write_handler is set
   WriteHandler write_handler;
   std::vector<uint8_t> write_chunk_; // Staged: the piece of the body nghttp2 is pointing into
   size_t offered_ = 0;
   size_t written_ = 0;
   bool is_deferred = false;

   //
   // ZeroCopy: set when a write was cancelled with bytes still outstanding, which resets the
   // stream -- so that close_stream() reports the cancellation to the handler it finds there,
   // rather than the connection_reset a reset would otherwise mean.
   //
   bool write_cancelled_ = false;

   //
   // How the end of the outgoing body travels: async_write_eof() sets \c eof_requested, and
   // data_reader() turns that into NGHTTP2_READ_DATA_FLAG_EOF on the very DATA frame that carries
   // the write's last bytes -- so a body that ends with data needs no second, empty frame.
   // \c eof_requested says the user has ended the body (only a bare re-end is accepted after
   // that), \c eof_submitted that nghttp2 has actually been told; between the two lies a
   // cancelled async_write_eof(), whose re-issue delivers the still-owed EOF flag.
   //
   bool eof_requested = false;
   bool eof_submitted = false;

   //
   // async_get_response()
   //
   client::Request::GetResponseHandler response_handler;

   /// Set to true after on_response(), which is invoked after the first headers have been received.
   bool has_response = false;

   /// Set to true after responseHandler has been invoked, to make sure that this happens only once.
   bool response_delivered = false;

   /// Set instead of has_response for a response that is not delivered, but fails.
   error_code response_error;

   /// Built on every call, not kept: only logging needs it, and a stream lives for one request.
   std::string log_prefix() const;
   std::string method;
   boost::urls::url url;

   //
   // Headers as they arrive from the peer. They are only collected while debug logging is on,
   // because they are logged as one block after the request or status line, which is only known
   // once all fields of the section have been seen. See log_received_headers().
   //
   std::vector<std::pair<std::string, std::string>> received_headers;
   std::optional<unsigned int> status_code;
   std::optional<size_t> content_length;
   Fields fields; // all received headers except the pseudo-headers

   /// Size of the received header section so far, see Config::max_header_size.
   size_t header_size = 0;
   bool header_limit_exceeded = false;

   bool closed = false; // set to true after the stream_close callback

   /// close_stream() was put off until the reader has taken the rest of the body, see there.
   bool close_deferred = false;

public:
   NGHttp2Stream(NGHttp2Session& parent, int64_t id, WriteMode write_mode);
   ~NGHttp2Stream();

   /// This function is called on receiving body data, `nghttp2_recv_data`.
   void on_data(const uint8_t* data, size_t len);

   /// This function is called once the peer has ended its side, `nghttp2_end_stream`.
   void on_eof();

   // =================================================================================================

   ReadSomeHandler read_handler_;
   boost::asio::mutable_buffer read_handler_buffer_;
   bool inside_call_read_handler_ = false;
   void call_read_handler(asio::const_buffer buffer = {});

   // ----------------------------------------------------------------------------------------------

   void async_write(WriteHandler handler, asio::const_buffer buffer, bool eof);

   void resume();

   void async_get_response(client::Request::GetResponseHandler&& handler);

   // ----------------------------------------------------------------------------------------------

   /// `nghttp2_read_data`: hand nghttp2 the part of \c write_buffer it has not seen yet.
   nghttp2_ssize data_reader(nghttp2_vec* vec, size_t veccnt, uint32_t* pflags);

   /// `nghttp2_write_stream_data_offset`: \p len bytes of the body went into a DATA frame.
   void on_write_offset(size_t len);

   void on_response();
   void deliver_response();
   void on_request();

   /// Log and discard the headers collected by the field callback.
   void log_received_headers();

   Reader::Impl* reader = nullptr;
   Writer::Impl* writer = nullptr;

   void delete_reader();
   void delete_writer();
   void maybe_close_stream();
   void finish_deferred_close();

   Executor get_executor() const noexcept;

public:
   NGHttp2Session& parent;
   int64_t id;
};

// =================================================================================================

/**
 * The `nghttp2_data_reader` through which nghttp2 pulls an outgoing body, by reference, out of
 * NGHttp2Stream::data_reader(). It needs no state of its own: the stream it belongs to arrives as
 * the per-stream user data, which both submit calls are given. nghttp2 copies the reader, so the
 * returned value does not have to outlive the call it is passed to.
 */
nghttp2_data_reader make_data_reader();

// =================================================================================================

} // namespace anyhttp::nghttp2
