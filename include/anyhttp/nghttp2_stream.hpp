#pragma once

#include "client.hpp"
#include "common.hpp"

#include "nghttp2/nghttp2.h"

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/system_category.hpp>

#include <boost/url/url.hpp>

#include <deque>

namespace anyhttp::nghttp2
{

// =================================================================================================

class NGHttp2Stream;

template <typename Base>
class NGHttp2Reader : public Base
{
public:
   explicit NGHttp2Reader(NGHttp2Stream& stream);
   ~NGHttp2Reader() override;

   const asio::any_io_executor& executor() const override;
   std::optional<size_t> content_length() const noexcept override;
   void async_read_some(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler) override;
   void detach() override;
   
   unsigned int status_code() const noexcept override;
   boost::url_view url() const override;

   NGHttp2Stream* stream;
};

// -------------------------------------------------------------------------------------------------

template <typename Base>
class NGHttp2Writer : public Base
{
public:
   explicit NGHttp2Writer(NGHttp2Stream& stream);
   ~NGHttp2Writer() override;

   const asio::any_io_executor& executor() const override;
   void content_length(std::optional<size_t> content_length) override;
   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override;
   void detach() override;

   void async_submit(WriteHandler&& handler, unsigned int status_code, const Fields& headers);
   void async_get_response(client::Request::GetResponseHandler&& handler);

   NGHttp2Stream* stream;
   std::optional<size_t> m_content_length;
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
    * added. But the might be still some buffers left to be deliver to the user.
    */
   bool eof_received = false;

   /**
    * Buffers received from peer, pending delivery to the user. The receiver tries to avoid
    * buffering if possible, but if there is currently no user-provided read handler to deliver
    * the data to, we have to store it -- decreasing the stream's receive window in the process.
    *
    * As long as \c m_pending_read_buffers is non-empty, \c m_read_buffer is viewing the part of
    * the first pending read buffer that has not been delivered, yet.
    *
    * If (and only if) the list of pending read buffers is empty, \c m_read_buffer may be empty as
    * well. While in \c call_read_handler(), it may be viewing the newly received data that has not
    * been added to the pending read buffers, yet.
    */
   asio::const_buffer m_read_buffer;
   using Buffer = std::vector<uint8_t>;
   std::deque<Buffer> m_pending_read_buffers;

   inline Buffer make_buffer(asio::const_buffer buffer)
   {
      return Buffer(static_cast<const uint8_t*>(buffer.data()),
                    static_cast<const uint8_t*>(buffer.data()) + asio::buffer_size(buffer));
   }

   static inline bool is_empty(asio::const_buffer buffer)
   {
      return asio::buffer_size(buffer) == 0;
   }

   /**
    * Returns true if all data has been read by the user.
    * This is true if there was an EOF flag and all buffers have been consumed.
    */
   inline bool reading_finished() const { return !reader || eof_received && is_empty(m_read_buffer); }

   /// Returns true if the user has submitted EOF and this has been delivered to nghttp2.
   inline bool writing_finished() const { return !writer || eof_submitted; };

   /**
    * Returns the number of bytes left to read. This is the remaining part of the first buffer
    * and the sum of all following buffers.
    */
   size_t read_buffers_size() const;

   // ----------------------------------------------------------------------------------------------

   //
   // async_write()
   //
   asio::const_buffer write_buffer;
   WriteHandler write_handler;
   bool is_deferred = false;
   bool eof_submitted = false;

   //
   // async_get_response()
   //
   client::Request::GetResponseHandler response_handler;

   /// Set to true after on_response(), which is invoked after the first headers have been received.
   bool has_response = false;

   /// Set to true after responseHandler has been invoked, to make sure that this happens only once.
   bool response_delivered = false;

   std::string logPrefix;
   std::string method;
   boost::urls::url url;
   std::optional<unsigned int> status_code;
   std::optional<size_t> content_length;

   bool closed = false; // set to true after on_stream_close_callback

public:
   NGHttp2Stream(NGHttp2Session& parent, int id);
   ~NGHttp2Stream();

   /// This function is called from `nghttp2_on_data_chunk_recv_callback`.
   void on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len);

   /// This function is called after handling a header or data frame, if the EOF flag is set.
   void on_eof(nghttp2_session* session, int32_t id_);

   // =================================================================================================

   ReadSomeHandler m_read_handler;
   boost::asio::mutable_buffer m_read_handler_buffer;
   bool m_inside_call_read_handler = false;
   void call_read_handler(asio::const_buffer buffer = {});

#if 0
   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      //
      // Define a function object that contains the code to launch the asynchronous
      // operation. This is passed the concrete completion handler, followed by any
      // additional arguments that were passed through the call to async_initiate.
      //
      auto init = [&](ReadSomeHandler handler)
      {
         assert(!m_read_handler);
         if (reading_finished())
         {
            logw("[{}] async_read_some: stream already finished", logPrefix);
            handler(boost::asio::error::misc_errors::eof, std::vector<std::uint8_t>{});
            return;
         }
#if 1
         m_read_handler = std::move(handler);
#else
         // According to the rules for asynchronous operations, we need to track
         // outstanding work against the handler's associated executor until the
         // asynchronous operation is complete.
         auto work = boost::asio::make_work_guard(handler);

         // Launch the operation with a callback that will receive the result and
         // pass it through to the asynchronous operation's completion handler.
         m_read_handler = [handler = std::move(handler), work = std::move(work),
                           logPrefix = logPrefix](boost::system::error_code ec,
                                                  std::vector<std::uint8_t> result) mutable
         {
            // Get the handler's associated allocator. If the handler does not
            // specify an allocator, use the recycling allocator as the default.
            auto alloc = boost::asio::get_associated_allocator(
               handler, boost::asio::recycling_allocator<void>());

            // Dispatch the completion handler through the handler's associated
            // executor, using the handler's associated allocator.
            logd("[{}] async_read_some: dispatching...", logPrefix);
            boost::asio::dispatch(
               work.get_executor(),
               boost::asio::bind_allocator(alloc, [handler = std::move(handler), ec,
                                                   result = std::move(result),
                                                   logPrefix = logPrefix]() mutable { //
                  logd("[{}] async_read_some: running dispatched handler...", logPrefix);
                  std::move(handler)(ec, result);
                  logd("[{}] async_read_some: running dispatched handler... done", logPrefix);
               }));
            logd("[{}] async_read_some: dispatching... done", logPrefix);
         };
         logd("[{}] async_read_some: read handler set", logPrefix);
#endif
         call_read_handler();
      };

      // The async_initiate function is used to transform the supplied completion
      // token to the completion handler. When calling this function we explicitly
      // specify the completion signature of the operation. We must also return the
      // result of the call since the completion token may produce a return value,
      // such as a future.
      return boost::asio::async_initiate<CompletionToken, ReadSome>(
         init, // First, pass the function object that launches the operation,
         token); // then the completion token that will be transformed to a handler.
   }
#endif

   // ----------------------------------------------------------------------------------------------

   void async_write(WriteHandler handler, asio::const_buffer buffer);

   void resume();

   void async_get_response(client::Request::GetResponseHandler&& handler);

   // ----------------------------------------------------------------------------------------------

   ssize_t producer_callback(uint8_t* buf, size_t length, uint32_t* data_flags);

   void on_response();
   void deliver_response();
   void on_request();

   impl::Reader* reader = nullptr;
   impl::Writer* writer = nullptr;

   void delete_reader();
   void delete_writer();
   void maybe_close_stream();

   const asio::any_io_executor& executor() const;

public:
   NGHttp2Session& parent;
   int id;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
