
#include "anyhttp/nghttp2_stream.hpp"
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/request_handlers.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/detail/error_code.hpp>

#include <nghttp2/nghttp2.h>

#include <type_traits>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Base>
NGHttp2Reader<Base>::NGHttp2Reader(NGHttp2Stream& stream) : Base(), stream(&stream)
{
   if constexpr (std::is_same_v<Base, server::Request::Impl>)
      stream.request = this;
}

template <typename Base>
NGHttp2Reader<Base>::~NGHttp2Reader()
{
   if (stream)
   {
      stream->request = nullptr;
      // stream->delete_reader();
      // stream->call_handler_loop();
   }
}

template <typename Base>
void NGHttp2Reader<Base>::detach()
{
   stream = nullptr;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
const asio::any_io_executor& NGHttp2Reader<Base>::executor() const
{
   assert(stream);
   return stream->executor();
}

template <typename Base>
boost::url_view NGHttp2Reader<Base>::url() const
{
   assert(stream);
   return {stream->url};
}

template <typename Base>
std::optional<size_t> NGHttp2Reader<Base>::content_length() const noexcept
{
   assert(stream);
   return stream->content_length;
}

template <typename Base>
void NGHttp2Reader<Base>::async_read_some(ReadSomeHandler&& handler)
{
   assert(stream);
   assert(!stream->m_read_handler);
   stream->m_read_handler = std::move(handler);
   stream->call_handler_loop();
}

// =================================================================================================

NGHttp2Writer::NGHttp2Writer(NGHttp2Stream& stream) : stream(&stream) { stream.response = this; }

NGHttp2Writer::~NGHttp2Writer()
{
   if (stream)
   {
      stream->response = nullptr;
      stream->delete_writer();
      // stream->call_handler_loop();
   }
}

void NGHttp2Writer::detach() { stream = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& NGHttp2Writer::executor() const
{
   assert(stream);
   return stream->executor();
}

void NGHttp2Writer::content_length(std::optional<size_t> content_length)
{
   m_content_length = content_length;
}

void NGHttp2Writer::async_submit(WriteHandler&& handler, unsigned int status_code, Fields headers)
{
   assert(stream);

   auto nva = std::vector<nghttp2_nv>();
   nva.reserve(3 + headers.size());

   std::string status_code_str = fmt::format("{}", status_code);
   nva.push_back(make_nv_ls(":status", status_code_str));
   std::string date = "Sat, 01 Apr 2023 09:33:09 GMT";
   nva.push_back(make_nv_ls("date", date));

   for (auto&& item : headers)
   {
      if (boost::iequals(item.first, "content-length") || item.first.starts_with(':'))
         logw("[{}] async_submit: invalid header '{}'", stream->logPrefix, item.first);

      nva.push_back(make_nv_ls(item.first, item.second));
   }

   std::string length_str;
   if (m_content_length)
   {
      length_str = fmt::format("{}", *m_content_length);
      nva.push_back(make_nv_ls("content-length", length_str));
   }

   // TODO: If we already know that there is no body, don't set a producer.
   prd.source.ptr = stream;
   prd.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t
   {
      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
      assert(stream);
      return stream->read_callback(buf, length, data_flags);
   };

   nghttp2_submit_response(stream->parent.session, stream->id, nva.data(), nva.size(), &prd);
   std::move(handler)(boost::system::error_code{});
}

void NGHttp2Writer::async_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (!stream)
      std::move(handler)(boost::asio::error::basic_errors::connection_aborted);
   else
      stream->async_write(buffer, std::move(handler));
}

void NGHttp2Writer::async_get_response(client::Request::GetResponseHandler&& handler)
{
   stream->async_get_response(std::move(handler));
}

// =================================================================================================

NGHttp2Stream::NGHttp2Stream(NGHttp2Session& parent, int id_)
   : parent(parent), id(id_), logPrefix(fmt::format("{}.{}", parent.logPrefix(), id_))
{
   logd("[{}] \x1b[1;33mStream: ctor\x1b[0m", logPrefix);
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

      boost::system::error_code ec;
      assert(!is_reading_finished);
      if (buffer_length)
      {
         bytesRead += buffer_length;
         if (content_length && bytesRead > *content_length)
         {
            logw("[{}] read_callback: received {} bytes, more than content length of {}", //
                 logPrefix, bytesRead, *content_length);
            // reset stream?
         }
      }
      else
      {
         is_reading_finished = true;
         if (content_length && *content_length != bytesRead)
         {
            logw("[{}] read_callback: EOF after {} bytes, less than content length of {}",
                 logPrefix, bytesRead, *content_length);
            ec = boost::beast::http::error::partial_message;
         }
      }

      logd("[{}] read_callback: calling handler with {} bytes...", logPrefix, buffer_length);
      std::move(handler)(ec, std::move(buffer));
      if (m_read_handler)
         logd("[{}] read_callback: calling handler with {} bytes... done,"
              " RESPAWNED ({} buffers pending)",
              logPrefix, buffer_length, m_pending_read_buffers.size());
      else
         logd("[{}] read_callback: calling handler with {} bytes... done", logPrefix,
              buffer_length);

      //
      // To apply back pressure, the stream is consumed only after the handler is invoked.
      //
      nghttp2_session_consume_stream(parent.session, id, buffer_length);
      parent.start_write();
   }

   logd("[{}] call_handler_loop: finished, {} buffers pending", logPrefix,
        m_pending_read_buffers.size());
}

void NGHttp2Stream::call_on_data(nghttp2_session* session, int32_t id_, const uint8_t* data,
                                 size_t len)
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
   logd("[{}] \x1b[33mStream: dtor\x1b[0m", logPrefix);
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
      // It is important to reset the deferred state before resuming, because this may result in
      // another call to the read callback.
      //
      is_deferred = false;
      nghttp2_session_resume_data(parent.session, id);
      parent.start_write();
   }
}

void NGHttp2Stream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   assert(!responseHandler);
   responseHandler = std::move(handler);
   if (has_response)
      call_on_response();
}

// ==============================================================================================

ssize_t NGHttp2Stream::read_callback(uint8_t* buf, size_t length, uint32_t* data_flags)
{
   logd("[{}] write callback (buffer size={} bytes)", logPrefix, length);
   assert(!is_deferred);
   assert(!is_writer_done);

   if (!sendHandler)
   {
      logd("[{}] write callback: no send handler, DEFERRING", logPrefix);
      is_deferred = true;
      return NGHTTP2_ERR_DEFERRED;
   }

   size_t copied = 0;
   if (sendBuffer.size())
   {
      //
      // TODO: we might be able to avoid copying by using NGHTTP2_DATA_FLAG_NO_COPY. This
      // will make nghttp2 call nghttp2_send_data_callback, which must emit a single, full
      // DATA frame.
      //
      copied = asio::buffer_copy(boost::asio::buffer(buf, length), sendBuffer);

      logd("[{}] write callback: copied {} bytes", logPrefix, copied);
      sendBuffer += copied;
      if (sendBuffer.size() == 0)
      {
         logd("[{}] write callback: running handler...", logPrefix);
         auto handler = std::move(sendHandler);
         std::move(handler)(boost::system::error_code{});
         if (sendHandler)
            logd("[{}] write callback: running handler... done -- RESPAWNED", logPrefix);
         else
            logd("[{}] write callback: running handler... done", logPrefix);
      }

      logd("[{}] write callback: {} bytes left", logPrefix, sendBuffer.size());
   }
   else
   {
      logd("[{}] write callback: EOF", logPrefix);
      is_writer_done = true;
      std::move(sendHandler)(boost::system::error_code{});
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
   }

   return copied;
}

void NGHttp2Stream::call_on_response()
{
   logd("[{}] call_on_response:", logPrefix);

   if (responseHandler)
   {
      auto impl = client::Response{std::make_unique<NGHttp2Reader<client::Response::Impl>>(*this)};
      std::move(responseHandler)(boost::system::error_code{}, std::move(impl));
      responseHandler = nullptr;
   }
   else
   {
      logw("[{}] call_on_response: not waiting for a response, yet", logPrefix);
      has_response = true;
   }
}

void NGHttp2Stream::call_on_request()
{
   logd("[{}] call_on_request: {}", logPrefix, url.buffer());

   //
   // An incoming new request should be put into a queue of the server session. From there,
   // new requests can then be retrieved asynchronously by the user.
   //
   // Setup of request and response should happen before that, too.
   //
   // TODO: Implement request queue. Until then, separate preparation of request/response from
   //       the actual handling.
   //
   server::Request request(std::make_unique<NGHttp2Reader<server::Request::Impl>>(*this));
   server::Response response(std::make_unique<NGHttp2Writer>(*this));

   auto& server = dynamic_cast<ServerReference&>(parent).server();
   if (auto& handler = server.requestHandlerCoro())
      co_spawn(executor(), handler(std::move(request), std::move(response)), detached);
   else if (auto& handler = server.requestHandler())
      server.requestHandler()(std::move(request), std::move(response));
   else
   {
      loge("[{}] call_on_request: no request handler!", logPrefix);
      co_spawn(executor(), not_found(std::move(request), std::move(response)), detached);
   }
}

const asio::any_io_executor& NGHttp2Stream::executor() const { return parent.executor(); }

// =================================================================================================

void NGHttp2Stream::delete_reader()
{
   logd("[{}] delete_reader", logPrefix);
   // Issue RST_STREAM so that stream does not hang around.
   // nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_INTERNAL_ERROR);
   // parent.start_write();
}

void NGHttp2Stream::delete_writer()
{
   logd("[{}] delete_writer", logPrefix);

   //
   // If the writer is deleted before it has delivered all data, we have to close the stream
   // so that it does not hang around
   //
   if (!is_writer_done)
   {
      logw("[{}] delete_writer: not done yet, submitting RST with STREAM_CLOSED", logPrefix);
      nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_STREAM_CLOSED);
      parent.start_write();
   }
}

// =================================================================================================

} // namespace anyhttp::nghttp2
