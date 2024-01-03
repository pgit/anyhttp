#pragma once

#include "common.hpp"
#include "session.hpp"

#include <deque>

#include <boost/asio.hpp>

#include "nghttp2/nghttp2.h"

namespace anyhttp::server
{

class Stream : public std::enable_shared_from_this<Stream>
{
public:
   size_t bytesRead = 0;
   size_t bytesWritten = 0;
   size_t pending = 0;
   size_t unhandled = 0;

   using Buffer = std::vector<uint8_t>;
   std::deque<Buffer> m_pending_read_buffers;

   std::vector<uint8_t> sendBuffer;
   asio::any_completion_handler<void()> sendHandler;
   bool is_deferred = false;

   std::string logPrefix;

   Stream(Session& parent, int id_)
      : parent(parent), id(id_), logPrefix(fmt::format("{}.{}", parent.logPrefix(), id_))
   {
      logd("[{}] Stream: ctor", logPrefix);
   }

   void call_handler_loop()
   {
      if (!m_read_handler)
      {
         size_t bytes = 0;
         for (const auto& buf : m_pending_read_buffers)
            bytes += buf.size();
         logd("[{}] read_callback: no pending read handler... ({} buffers and {} bytes pending)",
              logPrefix, m_pending_read_buffers.size(), bytes);
         return;
      }

      while (!m_pending_read_buffers.empty() && m_read_handler)
      {
         auto handler = std::move(m_read_handler);
         assert(!m_read_handler);

         auto buffer = std::move(m_pending_read_buffers.front());
         m_pending_read_buffers.pop_front();
         auto buffer_length = buffer.size();

         logd("[{}] read_callback: calling handler with {} bytes...", logPrefix, buffer_length);
         std::move(handler)(std::move(buffer));
         if (m_read_handler)
            logd("[{}] read_callback: READ HANDLER RESPAWNED!!!! ({} pending)", logPrefix,
                 m_pending_read_buffers.size());
         logd("[{}] read_callback: calling handler with {} bytes... done", logPrefix,
              buffer_length);

         //
         // To apply back pressure, the stream is consumed after the handler is invoked.
         //
         nghttp2_session_consume_stream(parent.session, id, buffer_length);
         parent.start_write();
      }

      logd("[{}] call_handler_loop: finished, {} buffers pending", logPrefix,
           m_pending_read_buffers.size());
   }

   void call_on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len)
   {
      std::ignore = id_;
      assert(id == id_);
      logd("[{}] read callback: {} bytes...", logPrefix, len);

      if (len)
         nghttp2_session_consume_connection(session, len);

      m_pending_read_buffers.emplace_back(data, data + len); // copy
      call_handler_loop();
   }

   ~Stream() { logd("[{}] Stream: dtor", logPrefix); }

   // ==============================================================================================

   // std::function<void(std::vector<std::uint8_t>)> m_read_handler;
   asio::any_completion_handler<void(std::vector<std::uint8_t>)> m_read_handler;

   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <boost::asio::completion_token_for<void(std::vector<std::uint8_t>)> CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      assert(!m_read_handler);

      // Define a function object that contains the code to launch the asynchronous
      // operation. This is passed the concrete completion handler, followed by any
      // additional arguments that were passed through the call to async_initiate.
      auto init = [&](asio::completion_handler_for<void(std::vector<std::uint8_t>)> auto handler)
      {
         // According to the rules for asynchronous operations, we need to track
         // outstanding work against the handler's associated executor until the
         // asynchronous operation is complete.
         auto work = boost::asio::make_work_guard(handler);

         assert(!m_read_handler);

         // Launch the operation with a callback that will receive the result and
         // pass it through to the asynchronous operation's completion handler.
         m_read_handler = [handler = std::move(handler), work = std::move(work),
                           logPrefix = logPrefix](std::vector<std::uint8_t> result) mutable
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
               boost::asio::bind_allocator(alloc, [handler = std::move(handler),
                                                   result = std::move(result),
                                                   logPrefix = logPrefix]() mutable { //
                  logd("[{}] async_read_some: running dispatched handler...", logPrefix);
                  std::move(handler)(result);
                  logd("[{}] async_read_some: running dispatched handler... done", logPrefix);
               }));
            logd("[{}] async_read_some: dispatching... done", logPrefix);
         };

         logd("[{}] async_read_some: read handler set", logPrefix);
         call_handler_loop();
      };

      // The async_initiate function is used to transform the supplied completion
      // token to the completion handler. When calling this function we explicitly
      // specify the completion signature of the operation. We must also return the
      // result of the call since the completion token may produce a return value,
      // such as a future.
      return boost::asio::async_initiate<CompletionToken, void(std::vector<std::uint8_t>)>(
         init, // First, pass the function object that launches the operation,
         token); // then the completion token that will be transformed to a handler.
   }

   // ----------------------------------------------------------------------------------------------

   template <boost::asio::completion_token_for<void()> CompletionToken>
   auto async_write(std::vector<std::uint8_t> buffer, CompletionToken&& token)
   {
      auto init =
         [&](asio::completion_handler_for<void()> auto handler, std::vector<uint8_t> buffer)
      {
         assert(!sendHandler);

         auto work = boost::asio::make_work_guard(handler);

         logd("[{}] async_write: buffer={} is_deferred={}", logPrefix, buffer.size(), is_deferred);

         sendBuffer = std::move(buffer);
         sendBufferView = boost::asio::buffer(sendBuffer);
         sendHandler =
            [handler = std::move(handler), work = std::move(work), logPrefix = logPrefix]() mutable
         {
            auto alloc = boost::asio::get_associated_allocator(
               handler, boost::asio::recycling_allocator<void>());

            logd("[{}] async_write: dispatching...", logPrefix);
            boost::asio::dispatch(
               work.get_executor(),
               boost::asio::bind_allocator(
                  alloc, [handler = std::move(handler), logPrefix = logPrefix]() mutable { //
                     logd("[{}] async_write: running dispatched handler...", logPrefix);
                     std::move(handler)();
                     logd("[{}] async_write: running dispatched handler... done", logPrefix);
                  }));
            logd("[{}] async_write: dispatching... done", logPrefix);
         };

         if (is_deferred)
         {
            logd("[{}] async_write: resuming session ({})", logPrefix, sendBuffer.size());

            //
            // It is important to reset this before resuming, because this may result in another
            // call to the read callback below.
            //
            is_deferred = false;
            nghttp2_session_resume_data(parent.session, id);
            parent.start_write();
         }
      };

      return boost::asio::async_initiate<CompletionToken, void()>(init, token, std::move(buffer));
   }

   // ==============================================================================================

   ssize_t read_callback(uint8_t* buf, size_t length, uint32_t* data_flags)
   {
      logd("[{}] write callback (buffer size={} bytes)", logPrefix, length);
      assert(!is_deferred);

      if (!sendHandler)
      {
         logd("[{}] write callback: no send handler, DEFERRING", logPrefix);
         is_deferred = true;
         return NGHTTP2_ERR_DEFERRED;
      }

      size_t copied = 0;
      if (!sendBuffer.empty())
      {
         assert(sendBufferView.data() >= sendBuffer.data());
         assert(sendBufferView.data() < sendBuffer.data() + sendBuffer.size());

         //
         // TODO: we might be able to avoid copying by using NGHTTP2_DATA_FLAG_NO_COPY. This
         // will make nghttp2 call nghttp2_send_data_callback, which must emit a single, full
         // DATA frame.
         //
         copied = asio::buffer_copy(boost::asio::buffer(buf, length), sendBufferView);

         logd("[{}] write callback: copied {} bytes", logPrefix, copied);
         if (copied == sendBufferView.size())
         {
            logd("[{}] write callback: running handler...", logPrefix);
            std::move(sendHandler)();
            logd("[{}] write callback: running handler... done", logPrefix);
            if (sendHandler)
            {
               logd("[{}] write callback: SEND HANDLER RESPAWNED", logPrefix);
               sendBufferView = boost::asio::buffer(sendBuffer);
            }
            else
            {
               sendBuffer.clear();
               sendBufferView = boost::asio::buffer(sendBuffer);
            }
         }
         else
         {
            sendBufferView += copied;
            assert(sendBufferView.size() > 0);
         }

         logd("[{}] write callback: {} bytes left", logPrefix, sendBufferView.size());
      }
      else
      {
         logd("[{}] write callback: EOF", logPrefix);
         std::move(sendHandler)();
         *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      }

      return copied;
   }

   awaitable<void> do_request()
   {
      logd("[{}] do_request", logPrefix);

      //
      // https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
      //
      // This callback is run by nghttp2 when it is ready to accept data to be sent.
      //
      nghttp2_data_provider prd;
      prd.source.ptr = this;
      prd.read_callback = [](nghttp2_session* session, int32_t stream_id, uint8_t* buf,
                             size_t length, uint32_t* data_flags, nghttp2_data_source* source,
                             void* user_data) -> ssize_t
      {
         std::ignore = session;
         std::ignore = stream_id;
         std::ignore = user_data;
         std::ignore = source;

         auto stream = static_cast<Stream*>(source->ptr);
         assert(stream);
         return stream->read_callback(buf, length, data_flags);
      };

      //
      // Submit response, full headers and producer callback for the body.
      //
      auto nva = std::vector<nghttp2_nv>();
      nva.reserve(2);
      std::string status = "200";
      std::string date = "Sat, 01 Apr 2023 09:33:09 GMT";
      nva.push_back(make_nv_ls(":status", status));
      nva.push_back(make_nv_ls("date", date));

      //
      // It is a little faster to just submit headers here and the response later, when there
      // is some data to send. Currently, there may be an extra DEFERRED round trip.
      //
      // auto rv = nghttp2_submit_response(parent.session, id, nva.data(), nva.size(), &prd);

      //
      // Async echo loop
      //
      size_t echoed = 0;
      bool headersSent = false;
      for (;;)
      {

         logd("[{}] CORO: async_read_some...", logPrefix);
         auto buffer = co_await async_read_some(deferred);
         logd("[{}] CORO: async_read_some... done", logPrefix);

         if (!headersSent)
         {
            logd("[{}] CORO: submitting response...", logPrefix);
            nghttp2_submit_response(parent.session, id, nva.data(), nva.size(), &prd);
            logd("[{}] CORO: submitting response... done", logPrefix);
            headersSent = true;
         }

         auto len = buffer.size();
         unhandled -= len;
         echoed += len;

         auto buffer_length = buffer.size();
         logd("[{}] CORO: echoing {} bytes...", logPrefix, buffer_length);
         co_await async_write(std::move(buffer), deferred);
         logd("[{}] CORO: echoing {} bytes... done", logPrefix, buffer_length);

         if (len == 0)
            break;
      }

      logd("[{}] CORO: echoed {} bytes total", logPrefix, echoed);
      co_return;
   }

   void call_on_request()
   {
      //
      // An incomming new request should be put into a queue of the server session. From there,
      // new requests can then be retrieved asynchronously by the user.
      //
      // Setup of request and response should happen before that, too.
      //
      // TODO: Implement request queue. Until then, separate preparation of request/response from
      //       the actual handling.
      //
      co_spawn(executor(), do_request(), detached);
   }

   inline const asio::any_io_executor& executor() const { return parent.executor(); }

public:
   Session& parent;
   boost::asio::const_buffer sendBufferView;
   int id;
};

} // namespace anyhttp::server