#pragma once

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/reader_impl.hpp"
#include "anyhttp/writer_impl.hpp"

#include "nghttp2/nghttp2.h"

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
   size_t bytesWritten = 0; // TODO: not used, yet
   size_t pending = 0; // TODO: not used, yet
   size_t unhandled = 0; // TODO: not used, yet

   // ----------------------------------------------------------------------------------------------

   /**
    * True after we have received an EOF flag from the peer. After this, no more buffers will be
    * added. But there might be still some buffers left to be deliver to the user.
    */
   bool eof_received = false;

   /**
    * Buffers received from peer, pending delivery to the user. The receiver tries to avoid
    * buffering if possible, but if there is currently no user-provided read handler to deliver
    * the data to, we have to store it -- decreasing the stream's receive window in the process.
    *
    * As long as \c pending_read_buffers_ is non-empty, \c read_buffer_ is viewing the part of
    * the first pending read buffer that has not been delivered, yet.
    *
    * If (and only if) the list of pending read buffers is empty, \c read_buffer_ may be empty as
    * well. While in \c call_read_handler(), it may be viewing the newly received data that has not
    * been added to the pending read buffers, yet.
    */
   asio::const_buffer read_buffer_;
   using Buffer = std::vector<uint8_t>;
   std::deque<Buffer> pending_read_buffers_;

   inline Buffer make_buffer(asio::const_buffer buffer)
   {
      return Buffer(static_cast<const uint8_t*>(buffer.data()),
                    static_cast<const uint8_t*>(buffer.data()) + asio::buffer_size(buffer));
   }

   static inline bool is_empty(asio::const_buffer buffer) { return asio::buffer_size(buffer) == 0; }

   /**
    * Returns true if all data has been read by the user.
    * This is true if there was an EOF flag and all buffers have been consumed.
    */
   inline bool reading_finished() const
   {
      return !reader || eof_received && is_empty(read_buffer_);
   }

   /**
    * Returns the number of bytes left to read. This is the remaining part of the first buffer
    * and the sum of all following buffers.
    */
   size_t read_buffers_size() const;

   // ----------------------------------------------------------------------------------------------

   //
   // async_write()
   //
   asio::const_buffer write_buffer; // undefined unless write_handler is set
   WriteHandler write_handler;
   bool is_deferred = false;

   //
   // How the end of the outgoing body travels: async_write_eof() sets \c eof_requested, and the
   // producer callback turns that into NGHTTP2_DATA_FLAG_EOF on the very DATA frame that carries
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

   std::string log_prefix_;
   const std::string& log_prefix() const noexcept { return log_prefix_; }
   std::string method;
   boost::urls::url url;

   //
   // Headers as they arrive from the peer. They are only collected while debug logging is on,
   // because they are logged as one block after the request or status line, which is only known
   // once all headers of the frame have been seen. See log_received_headers().
   //
   std::vector<std::pair<std::string, std::string>> received_headers;
   std::optional<unsigned int> status_code;
   std::optional<size_t> content_length;
   Fields fields; // all received headers except the pseudo-headers

   /// Size of the received header section so far, see Config::max_header_size.
   size_t header_size = 0;
   bool header_limit_exceeded = false;

   bool closed = false; // set to true after on_stream_close_callback

   /// close_stream() was put off until the reader has taken the rest of the body, see there.
   bool close_deferred = false;

public:
   NGHttp2Stream(NGHttp2Session& parent, int id);
   ~NGHttp2Stream();

   /// This function is called on receiving a data frame `nghttp2_on_data_chunk_recv_callback`.
   void on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len);

   /// This function is called on receiving a header or data frame that has the EOF flag set.
   void on_eof(nghttp2_session* session, int32_t id_);

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

   ssize_t producer_callback(uint8_t* buf, size_t length, uint32_t* data_flags);

   void on_response();
   void deliver_response();
   void on_request();

   /// Log and discard the headers collected by on_header_callback().
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
   int32_t id;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
