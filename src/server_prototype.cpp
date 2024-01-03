//
// Test alternative NGHTTP2 ASIO implementation with coroutines.
//
#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/cancellation_condition.hpp>
#include <boost/asio/experimental/co_composed.hpp>
#include <boost/asio/experimental/use_coro.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/detail/error_code.hpp>

#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "nghttp2/nghttp2.h"

#include <fmt/ostream.h>
#include <fmtlog.h>

using namespace std::chrono_literals;

namespace asio = boost::asio;

using tcp = asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using asio::awaitable;
using asio::co_spawn;
using asio::deferred;
using asio::detached;

using namespace boost::asio::experimental::awaitable_operators;

#include <nghttp2/asio_http2.h>
#include <nghttp2/asio_http2_server.h>
#include <nghttp2/nghttp2.h>

#include "detect_http2.hpp"

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<tcp::endpoint> : ostream_formatter
{
};

//
// https://www.boost.org/doc/libs/1_81_0/doc/html/boost_asio/reference/experimental__co_composed.html
//
template <typename AsyncReadStream, typename DynamicBuffer, typename CompletionToken>
auto async_detect_http2_client_preface_coro(AsyncReadStream& stream, DynamicBuffer& buffer,
                                            CompletionToken&& token)
{
   static_assert(asio::is_dynamic_buffer<DynamicBuffer>::value,
                 "DynamicBuffer type requirements not met");

   using namespace boost::asio;
   return async_initiate<CompletionToken, void(boost::system::error_code, size_t)>(
      experimental::co_composed<void(boost::system::error_code, bool)>(
         [](auto state, AsyncReadStream& stream, DynamicBuffer& buffer) -> void
         {
            try
            {
               state.throw_if_cancelled(true);
               state.reset_cancellation_state(enable_terminal_cancellation());

               //
               // FIXME: As a coroutine, we don't even need to loop like this.
               //        Instead, we could just read the minimum number of bytes needed for
               //        detection in one async call to async_read(). But we keep it like thiss for
               //        now, leaving it up to the detection function (is_http2_client_preface) to
               //        request as much additional buffer as it needs, dynamically.
               //
               for (;;)
               {
                  boost::tribool result =
                     boost::beast::detail::is_http2_client_preface(buffer.data());
                  if (!boost::indeterminate(result))
                     co_return {{}, static_cast<bool>(result)};

                  auto prepared = buffer.prepare(1460);
                  size_t n = co_await stream.async_read_some(prepared, deferred);
                  buffer.commit(n);
               }
            }
            catch (const boost::system::system_error& e)
            {
               std::cout << "ERROR: " << e.what() << std::endl;
               co_return {e.code(), false};
            }
         },
         stream),
      token, std::ref(stream), std::ref(buffer));
}

// inspired by <http://blog.korfuri.fr/post/go-defer-in-cpp/>, but our
// template can take functions returning other than void.
template <typename F, typename... T>
struct Defer
{
   explicit Defer(F&& f, T&&... t) : f(std::bind(std::forward<F>(f), std::forward<T>(t)...)) {}
   Defer(Defer&& o) noexcept : f(std::move(o.f)) {}
   ~Defer() { f(); }

   using ResultType = std::invoke_result_t<F, T...>;
   std::function<ResultType()> f;
};

template <typename F, typename... T>
Defer<F, T...> defer(F&& f, T&&... t)
{
   return Defer<F, T...>(std::forward<F>(f), std::forward<T>(t)...);
}

// ========================================================================================================

// Create nghttp2_nv from string literal |name| and std::string |value|.
template <size_t N>
nghttp2_nv make_nv_ls(const char (&name)[N], const std::string& value)
{
   return {(uint8_t*)name, (uint8_t*)value.c_str(), N - 1, value.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME};
}

template <class T>
constexpr auto make_string_view(const T* data, size_t len)
{
   return std::string_view(static_cast<const char*>(static_cast<const void*>(data)), len);
}

// -----------------------------------------------------------------------------------------------------

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

class Stream;
class Session
{
public:
   explicit Session(tcp::socket&& socket, asio::any_io_executor executor)
      : m_socket(std::move(socket)), m_executor(std::move(executor)),
        m_logPrefix(fmt::format("{}", m_socket.remote_endpoint()))

   {
      logd("[{}] new connection", m_logPrefix);
      m_send_buffer.resize(64 * 1024);
   }
   ~Session()
   {
      nghttp2_session_del(session);
      logd("[{}] connection destroyed", m_logPrefix);
   }

   awaitable<void> do_session();
   awaitable<void> send_loop(stream& stream);
   awaitable<void> recv_loop(stream& stream);

   asio::any_completion_handler<void()> m_send_handler;

   const auto& executor() const { return m_executor; }

   template <boost::asio::completion_token_for<void()> CompletionToken>
   auto async_wait_send(CompletionToken&& token)
   {
      auto init = [&](asio::completion_handler_for<void()> auto handler)
      {
         auto work = boost::asio::make_work_guard(handler);

         assert(!m_send_handler);
         m_send_handler = [handler = std::move(handler), work = std::move(work),
                           logPrefix = logPrefix()]() mutable
         {
            auto alloc = boost::asio::get_associated_allocator(
               handler, boost::asio::recycling_allocator<void>());

            //
            // Here, we do need a post -- otherwise, we end up calling
            // nghttp2_session_mem_send() from within a call to nghttp2_session_mem_recv().
            //
            logd("[{}] async_wait_send: posting...", logPrefix);
            boost::asio::dispatch(
               work.get_executor(),
               asio::bind_allocator(
                  alloc, [handler = std::move(handler), logPrefix = logPrefix]() mutable { //
                     logd("[{}] async_wait_send: running dispatched handler...", logPrefix);
                     std::move(handler)();
                     logd("[{}] async_wait_send: running dispatched handler... done", logPrefix);
                  }));
            logd("[{}] async_wait_send: posting... done", logPrefix);
         };
      };

      return boost::asio::async_initiate<CompletionToken, void()>(init, token);
   }

   void create_stream(int stream_id)
   {
      m_streams.emplace(stream_id, std::make_shared<Stream>(*this, stream_id));
      m_requestCounter++;
   }

   Stream* find_stream(int32_t stream_id)
   {
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
         return it->second.get();
      else
         return nullptr;
   }

   auto close_stream(int32_t stream_id)
   {
      std::shared_ptr<Stream> stream;
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
      {
         stream = it->second;
         m_streams.erase(it);
      }
      return stream;
   }

   void start_write()
   {
      if (!m_send_handler)
      {
         // logd("[{}] start_write: NO SEND HANDLER", m_logPrefix);
         return;
      }

      decltype(m_send_handler) handler;
      m_send_handler.swap(handler);
      logd("[{}] start_write: calling handler...", m_logPrefix);
      std::move(handler)();
      logd("[{}] start_write: calling handler... done", m_logPrefix);
   }

   const std::string& logPrefix() const { return m_logPrefix; }

public:
   nghttp2_session* session = nullptr;
   stream m_socket;

private:
   asio::any_io_executor m_executor;
   std::string m_logPrefix;

   std::vector<uint8_t> m_send_buffer;
   std::map<int, std::shared_ptr<Stream>> m_streams;

   size_t m_requestCounter = 0;
};

// -------------------------------------------------------------------------------------------------

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

// ========================================================================================================

int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   std::ignore = session;
   auto handler = static_cast<Session*>(user_data);

   if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
   {
      return 0;
   }

   handler->create_stream(frame->hd.stream_id);
   return 0;
}

//
// TODO: there is on_header_callback2, which can help in avoiding copying strings
//
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name,
                       size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags,
                       void* user_data)
{
   std::ignore = session;
   std::ignore = frame;
   std::ignore = flags;

   auto handler = static_cast<Session*>(user_data);
   logd("[{}] on_header_callback: {}={}", handler->logPrefix(), make_string_view(name, namelen),
        make_string_view(value, valuelen));
   return 0;
}

int on_frame_not_send_callback(nghttp2_session* session, const nghttp2_frame* frame,
                               int lib_error_code, void* user_data)
{
   if (frame->hd.type != NGHTTP2_HEADERS)
   {
      return 0;
   }
   std::ignore = lib_error_code;
   std::ignore = user_data;

   // Issue RST_STREAM so that stream does not hang around.
   nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                             NGHTTP2_INTERNAL_ERROR);

   return 0;
}

int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   std::ignore = session;

   auto handler = static_cast<Session*>(user_data);
   auto stream = handler->find_stream(frame->hd.stream_id);

   logd("[{}] on_frame_recv_callback: id={}", handler->logPrefix(), frame->hd.stream_id);

   switch (frame->hd.type)
   {
   case NGHTTP2_DATA:
      if (!stream)
      {
         logw("[{}] on_frame_recv_callback: DATA, but no stream found (id={})",
              handler->logPrefix(), frame->hd.stream_id);
         break;
      }

      logd("[{}] on_frame_recv_callback: DATA, flags={}", handler->logPrefix(), frame->hd.flags);
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
      {
         stream->call_on_data(session, frame->hd.stream_id, nullptr, 0);
      }

      break;

   case NGHTTP2_HEADERS:
   {
      if (!stream || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
      {
         break;
      }

      stream->call_on_request();
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
      {
         stream->call_on_data(session, frame->hd.stream_id, nullptr, 0);
      }

      handler->start_write();
      break;
   }
   }

   return 0;
}

int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id,
                                const uint8_t* data, size_t len, void* user_data)
{
   std::ignore = flags;

   auto handler = static_cast<Session*>(user_data);
   auto stream = handler->find_stream(stream_id);

   if (!stream)
   {
      logw("[{}] on_data_chunk_recv_callback: DATA, but no stream found (id={})",
           handler->logPrefix(), stream_id);
      return 0;
   }

   logd("[{}] on_frame_recv_callback: DATA, len={}", handler->logPrefix(), len);
   stream->call_on_data(session, stream_id, data, len);
   handler->start_write(); // might re-open windows

   return 0;
}

int on_frame_send_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   std::ignore = session;
   std::ignore = frame;

   auto handler = static_cast<Session*>(user_data);
   logd("[{}] on_frame_send_callback:", handler->logPrefix());
   return 0;
}

int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code,
                             void* user_data)
{
   std::ignore = session;
   std::ignore = error_code;

   auto handler = static_cast<Session*>(user_data);
   auto stream = handler->close_stream(stream_id);
   assert(stream);
   logd("[{}.{}] on_stream_close_callback:", handler->logPrefix(), stream_id);
   // post(stream->executor(), [stream]() {});
   return 0;
}

// ========================================================================================================

awaitable<void> Session::do_session()
{
   // m_socket.socket().set_option(tcp::no_delay(true));
   m_socket.set_option(tcp::no_delay(true));

   //
   // detect HTTP2 client preface, abort connection if not found
   //
   std::vector<uint8_t> data;
   auto buffer = boost::asio::dynamic_buffer(data);
   if (!co_await async_detect_http2_client_preface_coro(m_socket, buffer, deferred))
   {
      fmt::print("no HTTP2 client preface detected ({} bytes in buffer), closing connection\n",
                 buffer.size());

      auto socket = std::move(m_socket);
      socket.shutdown(asio::ip::tcp::socket::shutdown_send);
      socket.close();
      co_return;
   }

   //
   // setup nghttp2 callbacks
   //
   // https://github.com/kahlertl/pynghttp2/blob/main/pynghttp2/sessions.py#L390
   //     def establish_session(self):
   //        logger.debug('Connection from %s:%d', *self.peername)
   //        options = nghttp2.Options(no_auto_window_update=True, no_http_messaging=True)
   //        self.session = nghttp2.Session(nghttp2.session_type.SERVER, {
   //            'on_frame_recv': on_frame_recv,
   //            'on_data_chunk_recv': on_data_chunk_recv,
   //            'on_frame_send': on_frame_send,
   //            'on_stream_close': on_stream_close,
   //            'on_begin_headers': on_begin_headers,
   //            'on_header': on_header,
   //        }, user_data=self, options=options)
   //        self.session.submit_settings(self._settings)
   //
   nghttp2_session_callbacks* callbacks;
   auto rv = nghttp2_session_callbacks_new(&callbacks);
   if (rv != 0)
      throw std::runtime_error("nghttp2_session_callbacks_new");

   auto cb_del = defer(nghttp2_session_callbacks_del, callbacks);

   //
   // https://nghttp2.org/documentation/nghttp2_session_server_new.html
   //
   // At a minimum, send and receive callbacks need to be specified.
   //
   nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
   nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                             on_data_chunk_recv_callback);
   nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
   nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
   nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
   nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
   nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);

   //
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't do
   //
   nghttp2_option* options;
   nghttp2_option_new(&options);
   // nghttp2_option_set_no_http_messaging(options, 1);
   nghttp2_option_set_no_auto_window_update(options, 1);

   rv = nghttp2_session_server_new2(&session, callbacks, this, options);
   if (rv != 0)
      throw std::runtime_error("nghttp2_session_server_new");

   nghttp2_option_del(options);

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
   //
   rv = nghttp2_session_mem_recv(session, static_cast<const uint8_t*>(buffer.data().data()),
                                 buffer.size());
   if (rv < 0 || rv != buffer.size())
      throw std::runtime_error("nghttp2_session_mem_recv");

   //
   // send/receive loop
   //
   co_await (send_loop(m_socket) && recv_loop(m_socket));

   logd("[{}] session done", m_logPrefix);
}

// ----------------------------------------------------------------------------------------

//
// Implementing the send loop as a coroutine does not make much sense, as it may run out
// of work and then needs to wait on a channel to be activated again. Doing this with
// a normal, callback-based completion handler is probably easier.
//
awaitable<void> Session::send_loop(stream& stream)
{
   std::array<uint8_t, 64 * 1024> buffer;
   uint8_t *const p0 = buffer.data(), *const p1 = p0 + buffer.size(), *p = p0;
   for (;;)
   {
      const uint8_t* data; // data is valid until next call, so we don't need to copy it

      logd("[{}] send loop: nghttp2_session_mem_send...", m_logPrefix);
      const auto nread = nghttp2_session_mem_send(session, &data);
      logd("[{}] send loop: nghttp2_session_mem_send... {} bytes", m_logPrefix, nread);
      if (nread < 0)
      {
         logw("[{}] send loop: closing stream and throwing", m_logPrefix);
         stream.close(); // will also cancel the read loop
         throw std::runtime_error("nghttp2_session_mem_send");
      }

      //
      // If the new chunk still fits into the buffer, copy and send later.
      //
      if (nread && p + nread <= p1)
      {
         auto copied =
            asio::buffer_copy(asio::mutable_buffer(p, p1 - p), asio::buffer(data, nread));
         assert(nread == copied);
         p += nread;
         logd("[{}] send loop: buffered {} more bytes, total {}", m_logPrefix, nread, p - p0);
      }

      //
      // Is there anything to send in the buffer and/or the newly received chunk?
      //
      else if (const auto to_write = p - p0 + nread; to_write > 0)
      {
         const std::array<asio::const_buffer, 2> seq{asio::buffer(p0, p - p0),
                                                     asio::buffer(data, nread)};
         logd("[{}] send loop: writing {} bytes...", m_logPrefix, to_write);
         auto [ec, written] = co_await asio::async_write(stream, seq);
         if (ec)
         {
            loge("[{}] send loop: error writing {} bytes: {}", m_logPrefix, to_write, ec.message());
            break;
         }
         logd("[{}] send loop: writing {} bytes... done, wrote {}", m_logPrefix, to_write, written);
         assert(to_write == written);
         p = p0;
      }

      //
      // Wait for signal to start writing again.
      //
      else if (nread == 0)
      {
         if (nghttp2_session_want_write(session) && nghttp2_session_want_read(session))
            logd("[{}] send loop: session still wants to read and write", m_logPrefix);
         else if (nghttp2_session_want_write(session))
            logd("[{}] send loop: session still wants to write", m_logPrefix);
         else if (nghttp2_session_want_read(session))
            logd("[{}] send loop: session still wants to read", m_logPrefix);
         else
            break;

         logd("[{}] send loop: waiting...", m_logPrefix);
         co_await async_wait_send(deferred);
         logd("[{}] send loop: waiting... done", m_logPrefix);
      }
   }

   logd("[{}] send loop: done", m_logPrefix);
}

//
// The read loop is better suited for implementation as a coroutine, as it does not need to wait
// on re-activation by the user.
//
awaitable<void> Session::recv_loop(stream& stream)
{
   std::string reason("done");
   std::array<uint8_t, 64 * 1024> buffer;
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto [ec, n] = co_await stream.async_read_some(asio::buffer(buffer));
      if (ec)
      {
         logd("[{}] read: {}, terminating session", m_logPrefix, ec.message());
         reason = ec.message();
         nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
         break;
      }

      logd("");
      logd("[{}] read: nghttp2_session_mem_recv... ({} bytes)", m_logPrefix, n);
      const auto rv = nghttp2_session_mem_recv(session, buffer.data(), n);
      logd("[{}] read: nghttp2_session_mem_recv... done", m_logPrefix);
      logd("");

      if (rv < 0)
         throw std::runtime_error("nghttp2_session_mem_recv");

      start_write();
   }

   start_write();
   logi("[{}] recv loop: {}, served {} requests", m_logPrefix, reason, m_requestCounter);
}

// =================================================================================================

awaitable<void> listener()
{
   tcp::endpoint endpoint{tcp::v4(), 8080};

   auto executor = co_await boost::asio::this_coro::executor;
   tcp::acceptor acceptor(executor);
   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(endpoint);
   acceptor.listen();

   for (;;)
   {
      auto socket = co_await acceptor.async_accept(deferred);
      auto session = std::make_shared<Session>(std::move(socket), executor);
      co_spawn(executor, session->do_session(), [session](const std::exception_ptr&) {});
   }
}

// -------------------------------------------------------------------------------------------------

awaitable<void> log_loop()
{
   auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer(executor);
   for (;;)
   {
      fmtlog::poll();
      timer.expires_after(10ms);
      co_await timer.async_wait(deferred);
   }
}

// =================================================================================================

int main()
{
   fmtlog::setLogLevel(fmtlog::LogLevel::INF);
   fmtlog::flushOn(fmtlog::LogLevel::DBG);
   boost::asio::io_context io_context;

   boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](auto, auto)
      {
         fmt::print("\nINTERRUPTED\n");
         io_context.stop();
      });

   co_spawn(io_context, log_loop(), detached);
   co_spawn(io_context, listener(), detached);

   fmt::print("running IO context...\n");
   io_context.run();
   fmt::print("running IO context... done\n");
   return 0;
}

// =================================================================================================

//
// cmake --build build --target all -- && build/nghttp2_coro_6
// curl -v --http2-prior-knowledge --data-binary @CMakeLists.txt http://localhost:8080
// h2load http://localhost:8080 -d CMakeLists.txt -n 100 -c 10
//
