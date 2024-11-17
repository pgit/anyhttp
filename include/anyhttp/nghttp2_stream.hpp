#pragma once

#include "client_impl.hpp"
#include "common.hpp"
#include "server_impl.hpp"

#include "nghttp2/nghttp2.h"

#include <boost/asio.hpp>
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
   void async_read_some(server::Request::ReadSomeHandler&& handler) override;
   void detach() override;

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

   void async_submit(WriteHandler&& handler, unsigned int status_code, Fields headers);
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
   size_t bytesWritten = 0;
   size_t pending = 0;
   size_t unhandled = 0;

   using Buffer = std::vector<uint8_t>;
   std::deque<Buffer> m_pending_read_buffers;
   bool is_reading_finished = false;

   asio::const_buffer sendBuffer;
   WriteHandler sendHandler;
   asio::cancellation_slot slot;
   bool is_deferred = false;
   bool is_writer_done = false;

   client::Request::GetResponseHandler responseHandler;
   bool has_response = false;

   std::string logPrefix;
   std::string method;
   boost::urls::url url;
   std::optional<size_t> content_length;

   bool closed = false; // set to true after on_stream_close_callback

public:
   NGHttp2Stream(NGHttp2Session& parent, int id);
   ~NGHttp2Stream();

   void call_handler_loop();
   void call_on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len);

   // ==============================================================================================

   server::Request::ReadSomeHandler m_read_handler;
   bool m_inside_call_handler_loop = false;

   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      assert(!m_read_handler);

      //
      // Define a function object that contains the code to launch the asynchronous
      // operation. This is passed the concrete completion handler, followed by any
      // additional arguments that were passed through the call to async_initiate.
      //
      auto init = [&](ReadSomeHandler handler)
      {
         assert(!m_read_handler);
         if (is_reading_finished)
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
         call_handler_loop();
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

   // ----------------------------------------------------------------------------------------------

   void async_write(WriteHandler handler, asio::const_buffer buffer);

   // template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken>
   auto async_write(asio::const_buffer buffer, WriteHandler&& handler)
   {
   #if 1
      async_write(std::move(handler), buffer);
   #else
      return boost::asio::async_initiate<WriteHandler, Write>(
         [this](WriteHandler handler, asio::const_buffer buffer) {
            async_write(std::move(handler), std::move(buffer));
         }, handler, buffer);
   #endif
   }

   void resume();

   void async_get_response(client::Request::GetResponseHandler&& handler);

   // ==============================================================================================

   ssize_t read_callback(uint8_t* buf, size_t length, uint32_t* data_flags);

   void call_on_response();
   void call_on_request();

   impl::Reader* reader = nullptr;
   impl::Writer* writer = nullptr;

   void delete_reader();
   void delete_writer();

   const asio::any_io_executor& executor() const;

public:
   NGHttp2Session& parent;
   int id;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
