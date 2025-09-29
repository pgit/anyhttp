#include "anyhttp/nghttp2_stream.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/nghttp2_common.hpp"
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/request_handlers.hpp" // IWYU pragma: keep

#include <boost/algorithm/string/predicate.hpp>

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/http/error.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <nghttp2/nghttp2.h>

#include <ranges>
#include <utility>

using namespace boost::asio::experimental::awaitable_operators;

namespace errc = boost::system::errc;

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
   }
}

template <typename Base>
void NGHttp2Reader<Base>::detach()
{
   stream = nullptr;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
asio::any_io_executor NGHttp2Reader<Base>::get_executor() const noexcept
{
   assert(stream);
   return stream->get_executor();
}

template <typename Base>
unsigned int NGHttp2Reader<Base>::status_code() const noexcept
{
   assert(stream);
   return stream->status_code.value_or(0);
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
   if (!stream)
      return std::nullopt;
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

   //
   // Given an empty buffer, we can't do anything. This includes signalling EOF, because that is
   // done using an empty buffer as well. TODO: That design doesn't match the way ASIO usually
   // signals EOF, which is using asio::error::eof. We should do it like that, too.
   //
   if (asio::buffer_size(buffer) == 0)
   {
      std::move(handler)(boost::system::error_code{}, 0);
      return;
   }

#if 1
   auto cs = asio::get_associated_cancellation_slot(handler);
   if (cs.is_connected() && !cs.has_handler())
   {
      cs.assign([this](asio::cancellation_type_t ct)
      {
         logd("[{}] async_read_some: \x1b[1;31m{}\x1b[0m ({})", stream->logPrefix, "cancelled",
              int(ct));

         if (stream->m_read_handler)
         {
            swap_and_invoke(stream->m_read_handler, errc::make_error_code(errc::operation_canceled),
                            0);
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
   }
}

template <typename Base>
void NGHttp2Writer<Base>::detach()
{
   stream = nullptr;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
asio::any_io_executor NGHttp2Writer<Base>::get_executor() const noexcept
{
   assert(stream);
   return stream->get_executor();
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
   nghttp2_data_provider2 prd;
   prd.source.ptr = stream;
   prd.read_callback = [](nghttp2_session*, int32_t stream_id, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t
   {
      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
      assert(stream);
      assert(stream->id == stream_id);
      return stream->producer_callback(buf, length, data_flags);
   };

   nghttp2_submit_response2(stream->parent.session, stream->id, nva.data(), nva.size(), &prd);
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
   auto tail_sizes = m_pending_read_buffers | std::views::drop(1) |
                     std::views::transform([](const auto& buffer) { return buffer.size(); });

   auto sum_tail = std::ranges::fold_left(
      m_pending_read_buffers | std::views::drop(1) |
         std::views::transform([](const auto& buffer) { return buffer.size(); }),
      size_t{0}, [](size_t a, size_t b) { return a + b; });

   return asio::buffer_size(m_read_buffer) +
          std::ranges::fold_left(
             m_pending_read_buffers | std::views::drop(1) |
                std::views::transform([](const auto& buffer) { return buffer.size(); }),
             size_t{0}, [](size_t a, size_t b) { return a + b; });
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
   // TODO: Use errc::eof instead, like ASIO does.
   //
   else
   {
      if (eof_received && m_read_handler)
      {
         logd("[{}] read_callback: delivering EOF...", logPrefix);
         swap_and_invoke(m_read_handler, boost::system::error_code{}, 0);
         //
         // At this point, in testcases like "IgnoreRequest", the stream may already have been
         // deleted. This is because invoking the read handler eventually continues a coroutine,
         // which might drop request and response and by extension, the stream.
         //
         // To avoid this, deleting the stream is post()ed in on_stream_close_callback()
         //
         logd("[{}] read_callback: delivering EOF... done", logPrefix);
         return;
      }
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
   if (response_handler)
   {
      logw("Stream: dtor... cancelling async_get_response()", logPrefix);
      swap_and_invoke(response_handler, errc::make_error_code(errc::operation_canceled),
                      client::Response{nullptr});
   }
   if (m_read_handler)
   {
      logw("Stream: dtor... cancelling async_read_some()", logPrefix);
      swap_and_invoke(m_read_handler, boost::beast::http::error::partial_message, 0);
   }
   if (write_handler)
   {
      logw("Stream: dtor... cancelling async_write()", logPrefix);
      swap_and_invoke(write_handler, errc::make_error_code(errc::operation_canceled));
   }

   logd("[{}] \x1b[33mStream: dtor... done\x1b[0m", logPrefix);
}

// =================================================================================================

void NGHttp2Stream::async_write(WriteHandler handler, asio::const_buffer buffer)
{
   if (closed)
   {
      logw("[{}] async_write: stream already closed", logPrefix);
      std::move(handler)(errc::make_error_code(errc::operation_canceled));
      return;
   }

   assert(!write_handler);

   logd("[{}] async_write: buffer={} is_deferred={}", logPrefix, buffer.size(), is_deferred);

   assert(!write_handler);
   write_buffer = buffer;
   write_handler = std::move(handler);

   auto slot = asio::get_associated_cancellation_slot(write_handler);
   if (slot.is_connected() && !slot.has_handler())
   {
      slot.assign([this](asio::cancellation_type_t ct)
      {
         logd("[{}] async_write: \x1b[1;31m{}\x1b[0m ({})", logPrefix, "cancelled", int(ct));
         // delete_writer();

         if (write_handler)
         {
            using namespace boost::system::errc;
            swap_and_invoke(write_handler, make_error_code(operation_canceled));
         }
      });
   }

   resume();
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Stream::resume()
{
   if (is_deferred)
   {
      logd("[{}] async_write: resuming stream ({} bytes to write)", logPrefix, write_buffer.size());

      //
      // It is important to reset the deferred state before resuming, because that may result in
      // an immediate call to the read callback.
      //
      is_deferred = false;
      nghttp2_session_resume_data(parent.session, id);
      parent.start_write();
   }
}

// =================================================================================================

void NGHttp2Stream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   logd("[{}] async_get_response:", logPrefix);

   assert(!response_handler);
   if (response_delivered)
   {
      auto ec = asio::error::basic_errors::already_started;
      logw("[{}] async_get_response: \x1b[1;31m{}\x1b[0m", logPrefix, what(ec));
      std::move(handler)(ec, client::Response{nullptr});
      return;
   }

   auto cs = handler.get_cancellation_slot();
   if (cs.is_connected())
   {
      cs.assign([this](asio::cancellation_type_t ct)
      {
         logd("[{}] async_get_response: \x1b[1;31m{}\x1b[0m ({})", logPrefix, "cancelled", int(ct));

         if (response_handler)
         {
            // auto executor = get_associated_executor(response_handler, get_executor());
            post(get_executor(), [handler = std::move(response_handler)]() mutable
            {
               std::move(handler)(errc::make_error_code(errc::operation_canceled),
                                  client::Response{nullptr});
            });
         }
      });
   }

   response_handler = std::move(handler);
   deliver_response();
}

// -------------------------------------------------------------------------------------------------

//
// NOTE: The read callback of a nghttp2 data source is a write callback from our perspective.
// https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback2
//
ssize_t NGHttp2Stream::producer_callback(uint8_t* buf, size_t length, uint32_t* data_flags)
{
   if (closed)
   {
      logd("[{}] write callback: stream closed", logPrefix);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
   }

   assert(!is_deferred);
   assert(!eof_submitted);
   logd("[{}] write callback (buffer size={} bytes)", logPrefix, length);

   if (!write_handler)
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
   if (write_buffer.size())
   {
      //
      // TODO: we might be able to avoid copying by using NGHTTP2_DATA_FLAG_NO_COPY. This
      // will make nghttp2 call nghttp2_send_data_callback, which must emit a single, full
      // DATA frame.
      //
      copied = asio::buffer_copy(boost::asio::buffer(buf, length), write_buffer);

      logd("[{}] write callback: copied {} bytes", logPrefix, copied);
      write_buffer += copied;
      if (write_buffer.size() == 0)
      {
         logd("[{}] write callback: running handler...", logPrefix);
         swap_and_invoke(write_handler, boost::system::error_code{});
         if (write_handler)
            logd("[{}] write callback: running handler... done -- RESPAWNED", logPrefix);
         else
            logd("[{}] write callback: running handler... done", logPrefix);
      }

      logd("[{}] write callback: {} bytes left", logPrefix, write_buffer.size());
   }
   else
   {
      logd("[{}] write callback: EOF", logPrefix);
      eof_submitted = true;
      std::move(write_handler)(boost::system::error_code{});
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
   }

   return copied;
}

void NGHttp2Stream::on_response()
{
   logd("[{}] on_response:", logPrefix);
   has_response = true;
   deliver_response();
}

void NGHttp2Stream::deliver_response()
{
   if (!has_response)
   {
      logd("[{}] deliver_response: no response, yet", logPrefix);
   }
   else if (!response_handler)
   {
      logw("[{}] deliver_response: not waiting for a response, yet", logPrefix);
   }
   else
   {
      response_delivered = true;
      auto impl = client::Response{std::make_unique<NGHttp2Reader<client::Response::Impl>>(*this)};
      swap_and_invoke(response_handler, boost::system::error_code{}, std::move(impl));
   }
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Stream::on_request()
{
   logd("[{}] on_request: {}", logPrefix, url.buffer());

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
      co_spawn(get_executor(), handler(std::move(request), std::move(response)), detached);
   else if (auto& handler = server.requestHandler())
      server.requestHandler()(std::move(request), std::move(response));
   else
   {
      loge("[{}] on_request: no request handler!", logPrefix);
      co_spawn(get_executor(), not_found(std::move(response)), detached);
   }
}

asio::any_io_executor NGHttp2Stream::get_executor() const noexcept { return parent.get_executor(); }

// =================================================================================================

/**
 * This function is called if the "Request" (on the server side) is deleted. This means that we no
 * we no longer want to to read DATA from the peer. However, we might still might want to deliver a
 * response, like a 404. But if we reset the stream too early, this is stopped as well.
 *
 * The key is to NOT close the stream early, but rely on NGHTTP/2 flow control to eventually stop
 * any incoming DATA frames. The closing of the stream is delayed until after the response has been
 * delivered.
 *
 * On the client side, the same mechanism is applied.
 */
void NGHttp2Stream::delete_reader()
{
   logd("[{}] delete_reader", logPrefix);

   assert(!m_read_handler);

   if (closed)
      logd("[{}] delete_reader: stream already closed", logPrefix);
   else if (reading_finished())
      logd("[{}] delete_reader: reading finished", logPrefix);
   else if (size_t pending = read_buffers_size())
   {
      logw("[{}] delete_reader: reading not finished, discarding {} pending bytes in {} buffers",
           logPrefix, pending, m_pending_read_buffers.size());

      // Don't re-open the stream window here, or the peer will resume sending data to us.
      m_read_buffer = asio::const_buffer{};
      m_pending_read_buffers.clear();
   }

   reader = nullptr;
   maybe_close_stream();
}

void NGHttp2Stream::delete_writer()
{
   logd("[{}] delete_writer", logPrefix);

   //
   // If the writer is deleted before it has delivered all data, we have to close the stream
   // so that it does not hang around. There are a few design options:
   //
   // 1) Resetting the stream when the writer is deleted also means that we may not complete
   //    reading either. This is because there is no way to "half-close" to sending side with
   //    nghttp2.
   // 2) We could just leave the stream open and close it only when the reader has finished as well.
   //
   // Option 1) is preferred because it "fails early" and doesn't leave the sending side hanging
   // around, possibly causing timeouts. The user still has the option to keep the sending side
   // open if desired.
   //
   // For normal operation, the sender has to be end()ed anyway.
   //
   if (closed)
      logd("[{}] delete_writer: stream already closed", logPrefix);
   else if (eof_submitted)
      logd("[{}] delete_writer: writing finished", logPrefix);
   /*
   else
   {
      logw("[{}] delete_writer: not done yet, submitting RST with STREAM_CLOSED", logPrefix);
      nghttp2_submit_rst_stream(parent.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
      parent.start_write();
   }
   */

   writer = nullptr;
   maybe_close_stream();
}

void NGHttp2Stream::maybe_close_stream()
{
   if (closed)
      logd("[{}] cleanup_stream: stream already closed", logPrefix);
   else if (writer)
      logd("[{}] cleanup_stream: still writing", logPrefix);
   else if (!eof_submitted || (!eof_received && !reader))
   {
      logw("[{}] cleanup_stream: not reading or writing any more, "
           "submitting RST with STREAM_CLOSED",
           logPrefix);

      // submitting RST will lead to on_stream_close_callback(), eventually
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
