
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

#include <utility>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Base>
NGHttp2Reader<Base>::NGHttp2Reader(NGHttp2Stream& stream) : Base(), stream(&stream)
{
   stream.reader = this;
}

template <typename Base>
NGHttp2Reader<Base>::~NGHttp2Reader()
{
   if (stream)
   {
      stream->reader = nullptr;
      stream->delete_reader();
      stream->call_read_handler();
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
   if (!stream)
   {
      using namespace boost::system;
      std::move(handler)(boost::asio::error::operation_aborted, std::vector<uint8_t>{});
      return;
   }
   assert(!stream->m_read_handler);
   logd("[{}] async_read_some:", stream->logPrefix);
   stream->m_read_handler = std::move(handler);
   stream->call_read_handler();
}

// =================================================================================================

template <typename Base>
NGHttp2Writer<Base>::NGHttp2Writer(NGHttp2Stream& stream) : stream(&stream)
{
   stream.writer = this;
}

template <typename Base>
NGHttp2Writer<Base>::~NGHttp2Writer()
{
   if (stream)
   {
      stream->writer = nullptr;
      stream->delete_writer();
      // stream->call_handler_loop();
   }
}

template <typename Base>
void NGHttp2Writer<Base>::detach()
{
   stream = nullptr;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
const asio::any_io_executor& NGHttp2Writer<Base>::executor() const
{
   assert(stream);
   return stream->executor();
}

template <typename Base>
void NGHttp2Writer<Base>::content_length(std::optional<size_t> content_length)
{
   m_content_length = content_length;
}

template <typename Base>
void NGHttp2Writer<Base>::async_submit(WriteHandler&& handler, unsigned int status_code,
                                       Fields headers)
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
      // FIXME: why should content length be invalid?
      // if (boost::iequals(item.first, "content-length") || item.first.starts_with(':'))
      if (item.first.starts_with(':'))
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
   nghttp2_data_provider prd;
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

template <typename Base>
void NGHttp2Writer<Base>::async_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (!stream)
      std::move(handler)(boost::asio::error::basic_errors::connection_aborted);
   else
      stream->async_write(buffer, std::move(handler));
}

template <typename Base>
void NGHttp2Writer<Base>::async_get_response(client::Request::GetResponseHandler&& handler)
{
   stream->async_get_response(std::move(handler));
}

// =================================================================================================

NGHttp2Stream::NGHttp2Stream(NGHttp2Session& parent, int id_)
   : parent(parent), id(id_), logPrefix(fmt::format("{}.{}", parent.logPrefix(), id_))
{
   logd("[{}] \x1b[1;33mStream: ctor\x1b[0m", logPrefix);
}

void NGHttp2Stream::call_read_handler()
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

   //
   // Avoid recursion. In here, we invoke the read handler. This may resume a user-provided
   // coroutine, which is likely to call async_read_some() again. And this will lead to a call
   // to this function again...
   //
   if (m_inside_call_read_handler)
      return;

   Defer norecurse([this]() { m_inside_call_read_handler = false; });
   m_inside_call_read_handler = true;

   //
   // Try to deliver as many buffers as possible. As long as the consumer installs a new read
   // handler every time, consumption continues.
   //
   size_t consumed = 0;
   for (size_t count = 0; !m_pending_read_buffers.empty() && m_read_handler; ++count)
   {
      boost::system::error_code ec;
      assert(!is_reading_finished);

      auto buffer = std::move(m_pending_read_buffers.front());
      m_pending_read_buffers.pop_front();

      auto buffer_length = buffer.size();
      if (buffer_length)
      {
         bytesRead += buffer_length;
         if (content_length && bytesRead > *content_length)
         {
            logw("[{}] read_callback: received {} bytes total, exceeds content length of {}", //
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

      logd("[{}] read_callback: calling handler with {} bytes... (#{})", logPrefix, buffer_length,
           count);

      //
      // The read handler is moved into a local variable before it is called, so that a new read
      // handler may be set during its invocation.
      //
      decltype(m_read_handler) handler;
      m_read_handler.swap(handler);
      std::move(handler)(ec, std::move(buffer));

      if (m_read_handler)
         logd("[{}] read_callback: calling handler with {} bytes... done,"
              " RESPAWNED ({} buffers pending)",
              logPrefix, buffer_length, m_pending_read_buffers.size());
      else
         logd("[{}] read_callback: calling handler with {} bytes... done", logPrefix,
              buffer_length);

      consumed += buffer_length;
   }

   //
   // To apply back pressure, the stream is consumed AFTER the handler is invoked.
   //
   if (consumed)
   {
      nghttp2_session_consume_stream(parent.session, id, consumed);
      parent.start_write();
   }

   logd("[{}] call_handler_loop: finished, {} buffers pending", logPrefix,
        m_pending_read_buffers.size());

   if (closed && m_read_handler)
   {
      assert(m_pending_read_buffers.empty());
      assert(!is_reading_finished);
      logw("[{}] call_handler_loop: read after close", logPrefix);
      decltype(m_read_handler) handler;
      m_read_handler.swap(handler);
      std::move(handler)(boost::system::error_code{}, Buffer{});
      assert(!m_read_handler); // should not respawn
   }
}

void NGHttp2Stream::on_data(nghttp2_session* session, int32_t id_, const uint8_t* data,
                                 size_t len)
{
   std::ignore = id_;
   assert(id == id_);
   assert(len);  // EOF is handled by on_eof() instead
   logd("[{}] read callback: {} bytes...", logPrefix, len);

   if (len)
      nghttp2_session_consume_connection(session, len);

   //
   // FIXME: We might be able to avoid copying in some situations: If there are no pending buffers
   //        yet, and if there is already a pending read handler, we could invoke it directly.
   //
   m_pending_read_buffers.emplace_back(data, data + len); // FIXME: avoid copy
   call_read_handler();
}

void NGHttp2Stream::on_eof(nghttp2_session* session, int32_t id_)
{
   std::ignore = id_;
   assert(id == id_);
   logd("[{}] read callback: EOF...", logPrefix);

   m_pending_read_buffers.emplace_back(/* empty buffer*/);
   call_read_handler();
}


NGHttp2Stream::~NGHttp2Stream()
{
   logd("[{}] \x1b[33mStream: dtor... \x1b[0m", logPrefix);
   if (reader)
   {
      logd("Stream: dtor... detaching reader", logPrefix);
      reader->detach();
   }
   if (writer)
   {
      logd("Stream: dtor... detaching writer", logPrefix);
      writer->detach();
   }
   logd("[{}] \x1b[33mStream: dtor... done\x1b[0m", logPrefix);
}

// ==============================================================================================

void NGHttp2Stream::async_write(WriteHandler handler, asio::const_buffer buffer)
{
   if (closed)
   {
      logw("async_write: stream already closed", logPrefix);
      using namespace boost::system;
      std::move(handler)(errc::make_error_code(errc::operation_canceled));
      return;
   }

   assert(!sendHandler);

   logd("[{}] async_write: buffer={} is_deferred={}", //
        logPrefix, buffer.size(), is_deferred);

   sendBuffer = buffer;
   sendHandler = std::move(handler);

   slot = asio::get_associated_cancellation_slot(sendHandler);
   if (slot.is_connected() && !slot.has_handler())
   {
      slot.assign(
         [this](asio::cancellation_type_t ct)
         {
            logd("[{}] async_write: \x1b[1;31mcancelled\x1b[0m ({})", logPrefix, int(ct));
            delete_writer();

            if (sendHandler)
            {
               using namespace boost::system;
               invoke(sendHandler, errc::make_error_code(errc::operation_canceled));
            }
         });
   }

   resume();
}

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

//
// NOTE: The read callback of a nghttp2 data source is a write callback from our perspective.
// https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback2
//
ssize_t NGHttp2Stream::read_callback(uint8_t* buf, size_t length, uint32_t* data_flags)
{
   if (closed)
   {
      logd("[{}] write callback: stream closed", logPrefix);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
   }

   assert(!is_deferred);
   assert(!is_writer_done);
   logd("[{}] write callback (buffer size={} bytes)", logPrefix, length);

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
         invoke(sendHandler, boost::system::error_code{});
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
   server::Response response(std::make_unique<NGHttp2Writer<server::Response::Impl>>(*this));

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

   assert(!m_read_handler);

   // Issue RST_STREAM so that stream does not hang around.
   if (is_reading_finished)
      logd("[{}] delete_reader: reading already finished", logPrefix);
   else
   {
      for (auto& buffer : m_pending_read_buffers)
      {
         logd("[{}] delete_reader: discarding buffer of {} bytes", logPrefix, buffer.size());
         nghttp2_session_consume_stream(parent.session, id, buffer.size());
      }
      m_pending_read_buffers.clear();

      if (this->closed)
         logw("[{}] delete_reader: stream already closed", logPrefix);
      else
      {
         logw("[{}] delete_reader: not done yet, submitting RST with STREAM_CLOSED", logPrefix);
         // nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_FLOW_CONTROL_ERROR);
         nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_STREAM_CLOSED);
         parent.start_write();
      }
   }
}

void NGHttp2Stream::delete_writer()
{
   logd("[{}] delete_writer", logPrefix);

   //
   // If the writer is deleted before it has delivered all data, we have to close the stream
   // so that it does not hang around. There are a few design options:
   //
   // 1) Resetting the stream when the writer is deleted also means that we may notcomplete
   //    reading either.
   //
   if (!is_writer_done)
   {
      if (this->closed)
         logw("[{}] delete_writer: stream already closed", logPrefix);
      else
      {
         logw("[{}] delete_writer: not done yet, submitting RST with STREAM_CLOSED", logPrefix);
         nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_STREAM_CLOSED);
         parent.start_write();
      }
   }
}

// =================================================================================================

template class NGHttp2Reader<client::Response::Impl>;
template class NGHttp2Reader<server::Request::Impl>;
template class NGHttp2Writer<client::Request::Impl>;
template class NGHttp2Writer<server::Response::Impl>;

} // namespace anyhttp::nghttp2
