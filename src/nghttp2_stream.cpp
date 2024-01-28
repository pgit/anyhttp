
#include "anyhttp/nghttp2_stream.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <nghttp2/nghttp2.h>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

NGHttp2Request::NGHttp2Request(NGHttp2Stream& stream) : Impl(), stream(&stream)
{
   stream.request = this;
}

NGHttp2Request::~NGHttp2Request()
{
   if (stream)
      stream->request = nullptr;
}

void NGHttp2Request::detach() { stream = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& NGHttp2Request::executor() const
{
   assert(stream);
   return stream->executor();
}

void NGHttp2Request::async_read_some(ReadSomeHandler&& handler)
{
   assert(stream);
   assert(!stream->m_read_handler);
   stream->m_read_handler = std::move(handler);
   stream->call_handler_loop();
}

// =================================================================================================

NGHttp2Response::NGHttp2Response(NGHttp2Stream& stream) : stream(&stream)
{
   stream.response = this;
}

NGHttp2Response::~NGHttp2Response()
{
   if (stream)
      stream->response = nullptr;
}

void NGHttp2Response::detach() { stream = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& NGHttp2Response::executor() const
{
   assert(stream);
   return stream->executor();
}

void NGHttp2Response::write_head(unsigned int status_code, Headers headers)
{
   assert(stream);

   auto nva = std::vector<nghttp2_nv>();
   nva.reserve(2);
   std::string date = "Sat, 01 Apr 2023 09:33:09 GMT";
   nva.push_back(make_nv_ls(":status", fmt::format("{}", status_code)));
   nva.push_back(make_nv_ls("date", date));

   // TODO: headers
   std::ignore = headers;

   prd.source.ptr = stream;
   prd.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t
   {
      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
      assert(stream);
      return stream->read_callback(buf, length, data_flags);
   };

   nghttp2_submit_response(stream->parent.session, stream->id, nva.data(), nva.size(), &prd);
}

void NGHttp2Response::async_write(WriteHandler&& handler, std::vector<uint8_t> buffer)
{
   if (!stream)
      handler(boost::asio::error::basic_errors::connection_aborted);
   else
      stream->async_write(std::move(handler), std::move(buffer));
}

// =================================================================================================

NGHttp2Stream::NGHttp2Stream(NGHttp2Session& parent, int id_)
   : parent(parent), id(id_), logPrefix(fmt::format("{}.{}", parent.logPrefix(), id_))
{
   logd("[{}] Stream: ctor", logPrefix);
}

void NGHttp2Stream::call_handler_loop()
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
      // move read handler into local variable, it may be set again by the handler
      auto handler = std::move(m_read_handler);
      auto buffer = std::move(m_pending_read_buffers.front());
      m_pending_read_buffers.pop_front();
      auto buffer_length = buffer.size();

      assert(!is_reading_finished);
      if (buffer_length == 0)
         is_reading_finished = true;

      logd("[{}] read_callback: calling handler with {} bytes...", logPrefix, buffer_length);
      std::move(handler)(boost::system::error_code{}, std::move(buffer));
      if (m_read_handler)
         logd("[{}] read_callback: READ HANDLER RESPAWNED!!!! ({} pending)", logPrefix,
              m_pending_read_buffers.size());
      logd("[{}] read_callback: calling handler with {} bytes... done", logPrefix, buffer_length);

      //
      // To apply back pressure, the stream is consumed after the handler is invoked.
      //
      nghttp2_session_consume_stream(parent.session, id, buffer_length);
      parent.start_write();
   }

   logd("[{}] call_handler_loop: finished, {} buffers pending", logPrefix,
        m_pending_read_buffers.size());
}

void NGHttp2Stream::call_on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len)
{
   std::ignore = id_;
   assert(id == id_);
   logd("[{}] read callback: {} bytes...", logPrefix, len);

   if (len)
      nghttp2_session_consume_connection(session, len);

   m_pending_read_buffers.emplace_back(data, data + len); // copy
   call_handler_loop();
}

NGHttp2Stream::~NGHttp2Stream()
{
   logd("[{}] Stream: dtor", logPrefix);
   if (request)
      request->detach();
   if (response)
      response->detach();
}

// ==============================================================================================

void NGHttp2Stream::resume()
{
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
}

void NGHttp2Stream::async_write(WriteHandler&& handler, std::vector<uint8_t> buffer)
{
   assert(!sendHandler);
   sendHandler = std::move(handler);
   sendBuffer = std::move(buffer);
   sendBufferView = boost::asio::buffer(sendBuffer);

   if (is_deferred)
   {
      is_deferred = false;
      nghttp2_session_resume_data(parent.session, id);
      parent.start_write();
   }
}

// ==============================================================================================

ssize_t NGHttp2Stream::read_callback(uint8_t* buf, size_t length, uint32_t* data_flags)
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
         std::move(sendHandler)(boost::system::error_code{});
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
      std::move(sendHandler)(boost::system::error_code{});
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
   }

   return copied;
}

awaitable<void> NGHttp2Stream::do_request()
{
   logd("[{}] do_request", logPrefix);

   //
   // https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
   //
   // This callback is run by nghttp2 when it is ready to accept data to be sent.
   //
   nghttp2_data_provider prd;
   prd.source.ptr = this;
   prd.read_callback = [](nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source,
                          void* user_data) -> ssize_t
   {
      std::ignore = session;
      std::ignore = stream_id;
      std::ignore = user_data;
      std::ignore = source;

      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
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
   // is some data to send. Otherwise, there is an extra DEFERRED round trip.
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
      auto [ec, buffer] = co_await async_read_some(as_tuple(deferred));
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

void NGHttp2Stream::call_on_request()
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
#if 0
      co_spawn(executor(), do_request(), detached);
#else
   server::Request request(std::make_unique<NGHttp2Request>(*this));
   server::Response response(std::make_unique<NGHttp2Response>(*this));

   if (auto& handler = parent.server().requestHandlerCoro())
      co_spawn(executor(), handler(std::move(request), std::move(response)), detached);
   else if (auto& handler = parent.server().requestHandler())
      parent.server().requestHandler()(std::move(request), std::move(response));
   else
      co_spawn(executor(), do_request(), detached);
      // assert(false); // no requesthandler set
#endif
}

const asio::any_io_executor& NGHttp2Stream::executor() const { return parent.executor(); }

// =================================================================================================

} // namespace anyhttp::nghttp2
