
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/nghttp2_stream.hpp"
#include "anyhttp/client_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <nghttp2/nghttp2.h>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

#define mloge(x, ...) loge("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogd(x, ...) logd("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);

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

   auto handler = static_cast<NGHttp2Session*>(user_data);
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

   auto handler = static_cast<NGHttp2Session*>(user_data);
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

   auto handler = static_cast<NGHttp2Session*>(user_data);
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

   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_frame_send_callback:", handler->logPrefix());
   return 0;
}

int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code,
                             void* user_data)
{
   std::ignore = session;
   std::ignore = error_code;

   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = handler->close_stream(stream_id);
   assert(stream);
   logd("[{}.{}] on_stream_close_callback:", handler->logPrefix(), stream_id);
   // post(stream->executor(), [stream]() {});
   return 0;
}

// =================================================================================================

template <class T>
using nghttp2_unique_ptr = std::unique_ptr<T, void (*)(T*)>;

#define NGHTTP2_NEW(X)                                                                             \
   static nghttp2_unique_ptr<nghttp2_##X> nghttp2_##X##_new()                                      \
   {                                                                                               \
      nghttp2_##X* ptr;                                                                            \
      if (nghttp2_##X##_new(&ptr))                                                                 \
         throw std::runtime_error("nghttp2_" #X "_new");                                           \
      return {ptr, nghttp2_##X##_del};                                                             \
   }

NGHTTP2_NEW(session_callbacks)
NGHTTP2_NEW(option)

static auto setup_callbacks()
{
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
   auto callbacks_unique = nghttp2_session_callbacks_new();
   auto callbacks = callbacks_unique.get();

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

   return callbacks_unique;
}

// =================================================================================================

NGHttp2Session::NGHttp2Session(any_io_executor executor, ip::tcp::socket&& socket)
   : m_executor(std::move(executor)), m_socket(std::move(socket)),
     m_logPrefix(fmt::format("{}", normalize(m_socket.remote_endpoint())))
{
   mlogd("session created");
   m_send_buffer.resize(64 * 1024);
}

NGHttp2Session::NGHttp2Session(server::Server::Impl& parent, any_io_executor executor,
                               ip::tcp::socket&& socket)
   : NGHttp2Session(std::move(executor), std::move(socket))
{
   m_server = &parent;
}

NGHttp2Session::NGHttp2Session(client::Client::Impl& parent, any_io_executor executor,
                               ip::tcp::socket&& socket)
   : NGHttp2Session(std::move(executor), std::move(socket))
{
   m_client = &parent;
}

NGHttp2Session::~NGHttp2Session()
{
   m_streams.clear();
   mlogd("streams deleted");
   nghttp2_session_del(session);
   mlogd("session destroyed");
}

// =================================================================================================

awaitable<void> NGHttp2Session::do_server_session(std::vector<uint8_t> data)
{
   m_socket.set_option(ip::tcp::no_delay(true));
   auto callbacks = setup_callbacks();

   //
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't do
   //
   auto options = nghttp2_option_new();
   // nghttp2_option_set_no_http_messaging(options.get(), 1);
   nghttp2_option_set_no_auto_window_update(options.get(), 1);

   if (auto rv = nghttp2_session_server_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_server_new");

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
   //
   ssize_t rv = nghttp2_session_mem_recv(session, data.data(), data.size());
   if (rv < 0 || rv != data.size())
      throw std::runtime_error("nghttp2_session_mem_recv");

   data.clear();
   data.shrink_to_fit();

   //
   // send/receive loop
   //
   co_await (send_loop(m_socket) && recv_loop(m_socket));

   mlogd("server session done");
}

// -------------------------------------------------------------------------------------------------

awaitable<void> NGHttp2Session::do_client_session(std::vector<uint8_t> data)
{
   m_socket.set_option(ip::tcp::no_delay(true));
   auto callbacks = setup_callbacks();

   //
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't do
   //
   auto options = nghttp2_option_new();
   // nghttp2_option_set_no_http_messaging(options.get(), 1);
   nghttp2_option_set_no_auto_window_update(options.get(), 1);

   if (auto rv = nghttp2_session_client_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_client_new");

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);

   const uint32_t window_size = 256 * 1024 * 1024;
   std::array<nghttp2_settings_entry, 2> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size}}};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
   nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, window_size);

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
   //
   ssize_t rv = nghttp2_session_mem_recv(session, data.data(), data.size());
   if (rv < 0 || rv != data.size())
      throw std::runtime_error("nghttp2_session_mem_recv");

   data.clear();
   data.shrink_to_fit();

   //
   // send/receive loop
   //
   co_await (send_loop(m_socket) && recv_loop(m_socket));

   mlogd("client session done");
}

// -------------------------------------------------------------------------------------------------

client::Request NGHttp2Session::submit(boost::urls::url url, Headers headers) 
{
   return client::Request{nullptr};
}

// =================================================================================================

//
// Implementing the send loop as a coroutine does not make much sense, as it may run out
// of work and then needs to wait on a channel to be activated again. Doing this with
// a normal, callback-based completion handler is probably easier.
//
awaitable<void> NGHttp2Session::send_loop(stream& stream)
{
   std::array<uint8_t, 64 * 1024> buffer;
   uint8_t *const p0 = buffer.data(), *const p1 = p0 + buffer.size(), *p = p0;
   for (;;)
   {
      const uint8_t* data; // data is valid until next call, so we don't need to copy it

      mlogd("send loop: nghttp2_session_mem_send...");
      const auto nread = nghttp2_session_mem_send(session, &data);
      mlogd("send loop: nghttp2_session_mem_send... {} bytes", nread);
      if (nread < 0)
      {
         logw("send loop: closing stream and throwing");
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
         mlogd("send loop: buffered {} more bytes, total {}", nread, p - p0);
      }

      //
      // Is there anything to send in the buffer and/or the newly received chunk?
      //
      else if (const auto to_write = p - p0 + nread; to_write > 0)
      {
         const std::array<asio::const_buffer, 2> seq{asio::buffer(p0, p - p0),
                                                     asio::buffer(data, nread)};
         mlogd("send loop: writing {} bytes...", to_write);
         auto [ec, written] = co_await asio::async_write(stream, seq);
         if (ec)
         {
            mloge("send loop: error writing {} bytes: {}", to_write, ec.message());
            break;
         }
         mlogd("send loop: writing {} bytes... done, wrote {}", to_write, written);
         assert(to_write == written);
         p = p0;
      }

      //
      // Wait for signal to start writing again.
      //
      else if (nread == 0)
      {
         if (nghttp2_session_want_write(session) && nghttp2_session_want_read(session))
            mlogd("send loop: session still wants to read and write");
         else if (nghttp2_session_want_write(session))
            mlogd("send loop: session still wants to write");
         else if (nghttp2_session_want_read(session))
            mlogd("send loop: session still wants to read");
         else
            break;

         mlogd("send loop: waiting...");
         co_await async_wait_send(deferred);
         mlogd("send loop: waiting... done");
      }
   }

   mlogd("send loop: done");
}

// -------------------------------------------------------------------------------------------------

//
// The read loop is better suited for implementation as a coroutine, as it does not need to wait
// on re-activation by the user.
//
awaitable<void> NGHttp2Session::recv_loop(stream& stream)
{
   std::string reason("done");
   std::array<uint8_t, 64 * 1024> buffer;
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto [ec, n] = co_await stream.async_read_some(asio::buffer(buffer));
      if (ec)
      {
         mlogd("read: {}, terminating session", ec.message());
         reason = ec.message();
         nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
         break;
      }

      mlogd("");
      mlogd("read: nghttp2_session_mem_recv... ({} bytes)", n);
      const auto rv = nghttp2_session_mem_recv(session, buffer.data(), n);
      mlogd("read: nghttp2_session_mem_recv... done");

      if (rv < 0)
         throw std::runtime_error("nghttp2_session_mem_recv");

      start_write();
   }

   start_write();
   mlogi("recv loop: {}, served {} requests", reason, m_requestCounter);
}

// =================================================================================================

} // namespace anyhttp::nghttp2
