#include "anyhttp/nghttp2_stream.hpp"

#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/nghttp2_common.hpp"
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/request_handlers.hpp" // IWYU pragma: keep

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/http/error.hpp>

#include <range/v3/numeric/accumulate.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/transform.hpp>

#include <nghttp2/nghttp2.h>

#include <utility>

using namespace boost::asio::experimental::awaitable_operators;
namespace rv = ranges::views;

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Base>
NGHttp2Reader<Base>::NGHttp2Reader(NGHttp2Stream& stream) : stream(&stream)
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
void NGHttp2Reader<Base>::async_read_some(boost::asio::mutable_buffer buffer,
                                          ReadSomeHandler&& handler)
{
   if (!stream)
   {
      logw("[] async_read_some: stream already gone");
      //
      // FIXME: This may return the wrong error code in some situations. For example, in the
      //        cancellation testcases, it may happen that the stream gets destroyed before the
      //        user has seen the 'partial_message' error from async_read_some().
      //
      //        As a solution, we might need to store the error code in the reader, so it can be
      //        delivered on the next call to async_read_some().
      //
      //        It is still a bit unclear what should happen if the user calls async_read_some()
      //        after that. This is arguably misuse of the interface.
      //
      std::move(handler)(boost::beast::http::error::partial_message, 0);
      // std::move(handler)(boost::asio::error::operation_aborted, 0);
      return;
   }

   if (asio::buffer_size(buffer) == 0)
   {
      std::move(handler)(boost::system::error_code{}, 0);
      return;
   }

#if 1
   auto cs = asio::get_associated_cancellation_slot(handler);
   if (cs.is_connected() && !cs.has_handler())
   {
      cs.assign(
         [this](asio::cancellation_type_t ct)
         {
            logd("[{}] async_read_some: \x1b[1;31m"
                 "cancelled"
                 "\x1b[0m ({})",
                 stream->logPrefix, int(ct));

            if (stream->m_read_handler)
            {
               using namespace boost::system;
               swap_and_invoke(stream->m_read_handler,
                               errc::make_error_code(errc::operation_canceled), 0);
            }

            // stream->delete_writer();
         });
   }
#endif

   assert(!stream->m_read_handler);
   logd("[{}] async_read_some:", stream->logPrefix);
   stream->m_read_handler_buffer = buffer;
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
   logd("[{}] detach:", stream->logPrefix);

   //
   // FIXME: This causes ASAN errors in External.h2spec and a few others.
   //        Not sure why this is here anyway...
   //
#if 0
   if (stream->sendHandler)
      swap_and_invoke(stream->sendHandler, boost::asio::error::operation_aborted);
#endif
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
                                       const Fields& headers)
{
   if (!stream)
   {
      logw("[] async_submit: stream already gone");
      std::move(handler)(boost::asio::error::basic_errors::connection_aborted);
      return;
   }

   logd("[{}] async_submit: {}", stream->logPrefix, status_code);

   auto nva = std::vector<nghttp2_nv>();
   // nva.reserve(3 + headers.size());

   std::string status_code_str = std::format("{}", status_code);
   nva.push_back(make_nv_ls(":status", status_code_str));
   std::string date = format_http_date(std::chrono::system_clock::now());
   nva.push_back(make_nv_ls("date", date));

   for (auto&& item : headers)
   {
      // FIXME: why should content length be invalid?
      // if (boost::iequals(item.first, "content-length") || item.first.starts_with(':'))
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}'", stream->logPrefix, item.name_string());

      nva.push_back(make_nv_ls(item.name_string(), item.value()));
   }

   std::string length_str;
   if (m_content_length)
   {
      length_str = std::format("{}", *m_content_length);
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
   stream->parent.start_write();

   std::move(handler)(boost::system::error_code{});
}

template <typename Base>
void NGHttp2Writer<Base>::async_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (!stream)
   {
      logw("[] async_write: stream already gone");
      std::move(handler)(boost::asio::error::basic_errors::connection_aborted);
   }
   else
      stream->async_write(std::move(handler), buffer);
}

template <typename Base>
void NGHttp2Writer<Base>::async_get_response(client::Request::GetResponseHandler&& handler)
{
   if (!stream)
   {
      logw("[] async_get_response: stream already gone");
      std::move(handler)(boost::asio::error::basic_errors::connection_aborted,
                         client::Response{nullptr});
   }
   else
      stream->async_get_response(std::move(handler));
}

// =================================================================================================

NGHttp2Stream::NGHttp2Stream(NGHttp2Session& parent, int id_)
   : parent(parent), id(id_), logPrefix(std::format("{}.{}", parent.logPrefix(), id_))
{
   logd("[{}] \x1b[1;33mStream: ctor\x1b[0m", logPrefix);
}

size_t NGHttp2Stream::read_buffers_size() const
{
   return asio::buffer_size(m_read_buffer) + // this is a view of m_pending_read_buffers[0]
          ranges::accumulate(m_pending_read_buffers | rv::drop(1) |
                                rv::transform([](const auto& buffer) { return buffer.size(); }),
                             size_t(0));
}

void NGHttp2Stream::call_read_handler(asio::const_buffer view)
{
   if (!m_read_handler)
   {
      if (!is_empty(view))
      {
         m_pending_read_buffers.emplace_back(make_buffer(view));
         if (m_pending_read_buffers.size() == 1)
            m_read_buffer = asio::buffer(m_pending_read_buffers.front());
      }
      logd("[{}] read_callback: no pending read handler... ({} buffers and {} bytes pending)",
           logPrefix, m_pending_read_buffers.size(), read_buffers_size());
      return;
   }

   //
   // Avoid recursion. In this function, we invoke the read handler, which may resume a
   // user-provided coroutine. That coroutine is likely to call async_read_some() again,
   // resulting in another call to this function.
   //
   // This is an expected scenario in ASIO, similar to using dispatch() instead of post()
   // -- although ASIO also has some countermeasures against recursion I think.
   //
   if (m_inside_call_read_handler)
   {
      logd("[{}] read_callback: avoided recursion, returning...", logPrefix);
      return;
   }

   Defer no_recurse([&]() { m_inside_call_read_handler = false; });
   m_inside_call_read_handler = true;

   //
   // If there is no pending data to write, we can start writing the view right away.
   //
   if (m_pending_read_buffers.empty())
      std::swap(m_read_buffer, view); // view is empty after this

   //
   // Try to deliver as many buffers as possible. As long as the consumer installs a new read
   // handler after handling a buffer, this loop can continue.
   //
   size_t count = 0, consumed = 0;
   while (m_read_handler && !is_empty(m_read_buffer))
   {
      size_t copied = asio::buffer_copy(m_read_handler_buffer, m_read_buffer);
      count++, consumed += copied;
      bytesRead += copied;
      m_read_buffer += copied;

      logd("[{}] read_callback: calling read handler with {} bytes... (#{} in a row, buf={})",
           logPrefix, copied, count, asio::buffer_size(m_read_buffer));

      //
      // swap_and_invoke() moves the read handler into a local variable before invoking it,
      // allowing a new read handler to be set. This is what we call 'respawning' here.
      //
      swap_and_invoke(m_read_handler, boost::system::error_code{}, copied);

      if (m_read_handler)
         logd("[{}] read_callback: calling handler with {} bytes... done,"
              " RESPAWNED ({} buffers pending)",
              logPrefix, copied, m_pending_read_buffers.size());
      else
         logd("[{}] read_callback: calling handler with {} bytes... done", logPrefix, copied);

      //
      // Advance to next stored buffer. If there is none, start writing the view that has been
      // passed to this function.
      //
      if (is_empty(m_read_buffer))
      {
         if (!m_pending_read_buffers.empty())
            m_pending_read_buffers.pop_front();

         if (!m_pending_read_buffers.empty())
            m_read_buffer = asio::buffer(m_pending_read_buffers.front());
         else
            std::swap(m_read_buffer, view); // clear view so that we don't write it twice
      }
   }

   //
   // To apply back pressure, the stream is consumed AFTER the handler is invoked. As always, this
   // is accompanied by a start_write() because this might un-block flow control.
   //
   if (consumed)
   {
      nghttp2_session_consume_stream(parent.session, id, consumed);
      parent.start_write();
   }

   logd("[{}] read_callback: finished, {} buffers pending, eof_received={}", logPrefix,
        m_pending_read_buffers.size(), eof_received);

   //
   // Buffer remaining data from 'view' passed into this function.
   //
   if (!is_empty(m_read_buffer))
   {
      assert(!m_read_handler);
      if (m_pending_read_buffers.empty())
      {
         auto& buffer = m_pending_read_buffers.emplace_back(make_buffer(m_read_buffer));
         m_read_buffer = asio::buffer(buffer);
      }
      else if (!is_empty(view))
      {
         m_pending_read_buffers.emplace_back(make_buffer(view));
      }
   }

   //
   // Signal EOF if there is no more data to read.
   //
   else
   {
      if (eof_received && m_read_handler)
         swap_and_invoke(m_read_handler, boost::system::error_code{}, 0);
   }

   //
   // If there is no further user-provided read handler to call, we can return now.
   // The rest of this function is about delivering EOF or error codes.
   //
   if (!m_read_handler)
      return;

#if 1
   if (reading_finished() || closed)
      swap_and_invoke(m_read_handler, boost::beast::http::error::partial_message, 0);
#else
   if (reading_finished())
   {
      boost::system::error_code ec; //  = boost::asio::error::eof;
      if (content_length && bytesRead < *content_length)
      {
         logw("[{}] read_callback: EOF after {} bytes total,"
              " which less than advertised content length of {}",
              logPrefix, bytesRead, *content_length);
         ec = boost::beast::http::error::partial_message;
      }

      swap_and_invoke(m_read_handler, ec, 0);
   }
   else if (closed)
   {
      assert(m_pending_read_buffers.empty());
      // assert(!is_reading_finished);
      logw("[{}] call_handler_loop: read after close", logPrefix);
      swap_and_invoke(m_read_handler, boost::system::error_code{}, 0);
      assert(!m_read_handler); // FIXME -- but what if the user sets a new handler anyway?
   }
#endif
}

void NGHttp2Stream::on_data(nghttp2_session* session, int32_t id_, const uint8_t* data, size_t len)
{
   assert(id == id_);
   assert(!eof_received);
   assert(len); // for EOF, on_eof() is called instead

   logd("[{}] read callback: {} bytes...", logPrefix, len);

   if (len)
      nghttp2_session_consume_connection(session, len);

   //
   // We try to invoke the read handler directly with the new buffer view. Only if there is no
   // pending read handler to invoke, the data will be copied into a buffer.
   //
   call_read_handler(asio::const_buffer(data, len));
}

void NGHttp2Stream::on_eof(nghttp2_session* session, int32_t id_)
{
   assert(id == id_);
   assert(!eof_received);

   logd("[{}] read callback: EOF", logPrefix);

   eof_received = true;
   call_read_handler({/* empty buffer */});
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
      logw("[{}] async_write: stream already closed", logPrefix);
      using namespace boost::system;
      std::move(handler)(errc::make_error_code(errc::operation_canceled));
      return;
   }

   assert(!sendHandler);

   logd("[{}] async_write: buffer={} is_deferred={}", logPrefix, buffer.size(), is_deferred);

   assert(!sendHandler);
   sendBuffer = buffer;
   sendHandler = std::move(handler);

   auto slot = asio::get_associated_cancellation_slot(sendHandler);
   if (slot.is_connected() && !slot.has_handler())
   {
      slot.assign(
         [this](asio::cancellation_type_t ct)
         {
            logd("[{}] async_write: \x1b[1;31m"
                 "cancelled"
                 "\x1b[0m ({})",
                 logPrefix, int(ct));
            delete_writer();

            if (sendHandler)
            {
               using namespace boost::system::errc;
               swap_and_invoke(sendHandler, make_error_code(operation_canceled));
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
      // It is important to reset the deferred state before resuming, because that may result in
      // an immediate call to the read callback.
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
      logd("[{}] write callback: nothing to send, DEFERRING", logPrefix);
      is_deferred = true;
      return NGHTTP2_ERR_DEFERRED;
   }

   //
   // TODO: Try to avoid the extra round trip through this callback on EOF. Currently, EOF is
   //       signalled by an empty send buffer, but if that was done using an extra flag, we could
   //       return NGHTTP2_DATA_FLAG_EOF earlier.
   //
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
         swap_and_invoke(sendHandler, boost::system::error_code{});
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
      co_spawn(executor(), not_found(std::move(response)), detached);
   }
}

const asio::any_io_executor& NGHttp2Stream::executor() const { return parent.executor(); }

// =================================================================================================

/**
 * This function is called if the "Request" (on the server side) is deleted. This means that we no
 * we no longer want to to read DATA from the peer. However, we might still might want to deliver a
 * response, like a 404. But if we reset the stream too early, this doesn't work any more.
 *
 * The key is to NOT close the stream early, but rely on NGHTTP/2 flow control to eventually stop
 * any incoming DATA frames. The closing of the stream is delayed until after the response has been
 * delivered.
 */
void NGHttp2Stream::delete_reader()
{
   logd("[{}] delete_reader", logPrefix);

   assert(!m_read_handler);

   // Issue RST_STREAM so that stream does not hang around.
   if (reading_finished())
      logd("[{}] delete_reader: reading already finished", logPrefix);
   else
   {
      if (size_t pending = read_buffers_size())
      {
         logd("[{}] delete_reader: discarding {} pending bytes in {} buffers", logPrefix, pending,
              m_pending_read_buffers.size());

         nghttp2_session_consume_stream(parent.session, id, pending);
         m_read_buffer = asio::const_buffer{};
         m_pending_read_buffers.clear();
      }

      if (this->closed)
         logw("[{}] delete_reader: stream already closed", logPrefix);
      else
      {
#if 1
         logw("[{}] delete_reader: not done yet, keeping stream open", logPrefix);
         // if we do this, we have to close the stream later, see below
#else
         logw("[{}] delete_reader: not done yet, submitting RST with STREAM_CLOSED", logPrefix);
         // nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id,
         // NGHTTP2_FLOW_CONTROL_ERROR);
         nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_STREAM_CLOSED);
         parent.start_write();
#endif
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
   // 1) Resetting the stream when the writer is deleted also means that we may not complete
   //    reading either.
   // 2) We could just leave the stream open and close it only when the reader has finished as well.
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
   else if (!reader && !this->closed)
   {
      logw("[{}] delete_writer: no reader left, submitting RST with STREAM_CLOSED", logPrefix);
      nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_STREAM_CLOSED);
      parent.start_write();
   }
}

// =================================================================================================

template class NGHttp2Reader<client::Response::Impl>;
template class NGHttp2Reader<server::Request::Impl>;
template class NGHttp2Writer<client::Request::Impl>;
template class NGHttp2Writer<server::Response::Impl>;

} // namespace anyhttp::nghttp2
