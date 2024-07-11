
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/client.hpp"
#include "anyhttp/nghttp2_stream.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>

#include <charconv>
#include <nghttp2/nghttp2.h>
#include <string>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);

   logd("[{}] on_begin_header_callback:", handler->logPrefix());

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
   auto namesv = make_string_view(name, namelen);
   auto valuesv = make_string_view(value, valuelen);
   logd("[{}.{}] on_header_callback: {}={}", handler->logPrefix(), frame->hd.stream_id, namesv,
        valuesv);

   auto stream = handler->find_stream(frame->hd.stream_id);
   assert(stream);

   if (namesv == ":method")
      stream->method = valuesv;
   else if (namesv == ":path")
      stream->url.set_path(valuesv);
   else if (namesv == ":scheme")
      stream->url.set_scheme(valuesv);
   else if (namesv == ":authority")
      stream->url.set_encoded_authority(valuesv);
   else if (namesv == ":host")
      stream->url.set_host(valuesv);
   else if (namesv == "content-length")
   {
      stream->content_length.emplace();
      std::from_chars(valuesv.begin(), valuesv.end(), *stream->content_length);
   }

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

static std::string_view frameType(uint8_t type)
{
   switch (type)
   {
   case NGHTTP2_DATA:
      return "DATA";
   case NGHTTP2_HEADERS:
      return "HEADERS";
   case NGHTTP2_PRIORITY:
      return "PRIORITY";
   case NGHTTP2_RST_STREAM:
      return "RST_STREAMS";
   case NGHTTP2_SETTINGS:
      return "SETTINGS";
   case NGHTTP2_PUSH_PROMISE:
      return "PUSH_PROMISE";
   case NGHTTP2_PING:
      return "PING";
   case NGHTTP2_GOAWAY:
      return "GOAWAY";
   case NGHTTP2_WINDOW_UPDATE:
      return "WINDOW_UPDATE";
   case NGHTTP2_CONTINUATION:
      return "CONTINU";
   case NGHTTP2_ALTSVC:
      return "ALTSVC";
   case NGHTTP2_ORIGIN:
      return "ORIGIN";
   case NGHTTP2_PRIORITY_UPDATE:
      return "PRIOIRTY_UPDATE";
   default:
      return "UNKNOWN";
   }
}

int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   std::ignore = session;

   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = handler->find_stream(frame->hd.stream_id);

   if (frame->hd.stream_id)
      logd("[{}.{}] on_frame_recv_callback: {}", handler->logPrefix(), frame->hd.stream_id,
           frameType(frame->hd.type));
   else
      logd("[{}] on_frame_recv_callback: {}", handler->logPrefix(), frameType(frame->hd.type));

   switch (frame->hd.type)
   {
   case NGHTTP2_DATA:
      if (!stream)
      {
         logw("[{}] on_frame_recv_callback: DATA, but no stream found (id={})",
              handler->logPrefix(), frame->hd.stream_id);
         break;
      }

      logd("[{}.{}] on_frame_recv_callback: DATA, flags={}", handler->logPrefix(),
           frame->hd.stream_id, frame->hd.flags);
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
      {
         stream->call_on_data(session, frame->hd.stream_id, nullptr, 0);
      }

      break;

   case NGHTTP2_HEADERS:
   {
      if (!stream)
      {
         break;
      }

      if (frame->headers.cat == NGHTTP2_HCAT_REQUEST)
         stream->call_on_request();
      else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE)
         stream->call_on_response();

      // no body?
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
         stream->call_on_data(session, frame->hd.stream_id, nullptr, 0);

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
      logw("[{}.{}] on_data_chunk_recv_callback: DATA, but no stream found (id={})",
           handler->logPrefix(), stream_id, stream_id);
      return 0;
   }

   logd("[{}.{}] on_frame_recv_callback: DATA, len={}", handler->logPrefix(), stream_id, len);
   stream->call_on_data(session, stream_id, data, len);
   handler->start_write(); // might re-open windows

   return 0;
}

int on_frame_send_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   std::ignore = session;
   std::ignore = frame;

   auto type = frameType(frame->hd.type);

   auto handler = static_cast<NGHttp2Session*>(user_data);
   if (frame->hd.stream_id)
      logd("[{}.{}] on_frame_send_callback: {}", handler->logPrefix(), frame->hd.stream_id,
           frameType(frame->hd.type));
   else
      logd("[{}] on_frame_send_callback: {}", handler->logPrefix(), frameType(frame->hd.type));
   
   if (frame->hd.type == NGHTTP2_GOAWAY)
      logw("[{}] on_frame_send_callback: {}", handler->logPrefix(), frameType(frame->hd.type));
   
   return 0;
}

int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code,
                             void* user_data)
{
   std::ignore = session;
   std::ignore = error_code;

   auto sessionWrapper = static_cast<NGHttp2Session*>(user_data);
   logd("[{}.{}] on_stream_close_callback:", sessionWrapper->logPrefix(), stream_id);

   auto stream = sessionWrapper->close_stream(stream_id);
   assert(stream);

   if (stream->responseHandler)
   {
      using namespace boost::system;
      std::move(stream->responseHandler)(errc::make_error_code(errc::io_error), // FIXME:
                                         client::Response{nullptr});
      stream->responseHandler = nullptr;
   }

   if (stream->sendHandler)
   {
      using namespace boost::system;
      std::move(stream->sendHandler)(errc::make_error_code(errc::operation_canceled));
      stream->sendHandler = nullptr;
   }

   post(stream->executor(), [stream]() {});
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

NGHttp2Session::NGHttp2Session(std::string_view prefix, any_io_executor executor)
   : m_executor(std::move(executor)), m_logPrefix(prefix)
{
   mlogd("session created");
}

NGHttp2Session::NGHttp2Session(server::Server::Impl& parent, any_io_executor executor)
   : NGHttp2Session("\x1b[1;31mserver\x1b[0m", std::move(executor))
{
   m_server = &parent;
}

NGHttp2Session::NGHttp2Session(client::Client::Impl& parent, any_io_executor executor)
   : NGHttp2Session("\x1b[1;32mclient\x1b[0m", std::move(executor))
{
   m_client = &parent;
   create_client_session();
}

NGHttp2Session::~NGHttp2Session()
{
   m_streams.clear();
   mlogd("streams deleted");
   nghttp2_session_del(session);
   mlogd("session destroyed");
}

// =================================================================================================

awaitable<void> NGHttp2Session::do_server_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);
   // m_socket.set_option(ip::tcp::no_delay(true));
   auto callbacks = setup_callbacks();

   //
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't
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
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await (send_loop() && recv_loop());

   mlogd("server session done");
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Session::create_client_session()
{
   // m_socket.set_option(ip::tcp::no_delay(true));
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

   // const uint32_t window_size = 256 * 1024 * 1024;
   const uint32_t window_size = 1024 * 1024;
   std::array<nghttp2_settings_entry, 2> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size}}};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
   nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, window_size);
}

awaitable<void> NGHttp2Session::do_client_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await (send_loop() && recv_loop());

   mlogd("client session done");
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Session::async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers)
{
   mlogi("submit: {}", url.buffer());

   std::ignore = headers;

   auto stream = std::make_shared<NGHttp2Stream>(*this, 0);
   stream->url = url;

   //
   // https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
   //
   // This callback is run by nghttp2 when it is ready to accept data to be sent.
   //
   nghttp2_data_provider prd;
   prd.source.ptr = stream.get();
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
   // Submit request, full headers and producer callback for the body.
   //
   auto view = boost::urls::parse_uri(url);
   std::string method("POST");
   std::string scheme(view->scheme());
   std::string path(view->path());
   std::string authority(view->host_address());
   auto nva = std::vector<nghttp2_nv>();
   nva.reserve(4);
   nva.push_back(make_nv_ls(":method", method));
   nva.push_back(make_nv_ls(":scheme", scheme));
   nva.push_back(make_nv_ls(":path", path));
   nva.push_back(make_nv_ls(":authority", authority));
   for (auto nv : nva)
      mlogd("submit: {}={}", std::string_view(reinterpret_cast<const char*>(nv.name), nv.namelen),
            std::string_view(reinterpret_cast<const char*>(nv.value), nv.valuelen));

   auto id = nghttp2_submit_request(session, nullptr, nva.data(), nva.size(), &prd, this);
   stream->id = id;
   stream->logPrefix = fmt::format("{}.{}", logPrefix(), id);
   if (id < 0)
   {
      mloge("submit: nghttp2_submit_request: ERROR: {}", id);
      using namespace boost::system;
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
   }

   logd("submit: stream={}", id);
   m_streams.emplace(id, stream);
   std::move(handler)(boost::system::error_code{},
                      client::Request{std::make_unique<NGHttp2Writer>(*stream)});
}

// =================================================================================================

#undef mlogd
#define mlogd(...) // too noisy

//
// Implementing the send loop as a coroutine does not make much sense, as it may run out
// of work and then needs to wait on a channel to be activated again. Doing this with
// a normal, callback-based completion handler is probably easier.
//
// This function calls nghttp2_session_mem_send() and collects the retrieved data in a send buffer,
// until either the buffer is full or no more data is returned. Then, the buffered data is written
// to the stream. Finally, if still no more data is returned, it waits for a signal to resume.
//
template <typename Stream>
awaitable<void> NGHTttp2SessionImpl<Stream>::send_loop()
{
   Buffer buffer;
   buffer.reserve(1460);
   for (;;)
   {
      //
      // Retrieve a chunk of data from NGHTTP2, to be sent.
      //
      const uint8_t* data; // data is valid until next call, so we don't need to copy it

      mlogd("send loop: nghttp2_session_mem_send...");
      const auto nread = nghttp2_session_mem_send(session, &data);
      mlogd("send loop: nghttp2_session_mem_send... {} bytes", nread);
      if (nread < 0)
      {
         logw("send loop: closing stream and throwing");
         m_stream.lowest_layer().close(); // will also cancel the read loop
         throw std::runtime_error("nghttp2_session_mem_send");
      }

      //
      // If the new chunk fits into the buffer, accumulate and send later.
      //
      if (nread && nread <= (buffer.capacity() - buffer.size()))
      {
         auto copied = asio::buffer_copy(buffer.prepare(nread), asio::buffer(data, nread));
         assert(nread == copied);
         buffer.commit(nread);
         mlogd("send loop: buffered {} more bytes, total {}", nread, buffer.data().size());
      }

      //
      // Is there anything to send in the buffer and/or the newly received chunk?
      // If yes, combine both into a buffer sequence and pass it to async_write().
      // Afterwards, go back up and ask NGHTTP2 if there is more data to send.
      //
      else if (const auto to_write = buffer.size() + nread; to_write > 0)
      {
         const std::array<asio::const_buffer, 2> seq{buffer.data(), asio::buffer(data, nread)};
         mlogd("send loop: writing {} bytes...", to_write);
         auto [ec, written] = co_await asio::async_write(m_stream, seq, as_tuple(deferred));
         if (ec)
         {
            mloge("send loop: error writing {} bytes: {}", to_write, ec.message());
            break;
         }
         mlogd("send loop: writing {} bytes... done, wrote {}", to_write, written);
         assert(to_write == written);
         buffer.consume(written);
      }

      //
      // Finally, if there is really nothing to send any more, wait for a signal to start again.
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
            break; // nghttp2 doesn't want to send or receive any more, so we are done

         mlogd("send loop: waiting...");
         co_await async_wait_send(deferred);
         mlogd("send loop: waiting... done");
      }
   }

   mlogd("send loop: done");
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Session::handle_buffer_contents()
{
   mlogd("");
   mlogd("read: nghttp2_session_mem_recv... ({} bytes)", m_buffer.size());
   auto data = m_buffer.data();
   ssize_t rv = nghttp2_session_mem_recv(session, static_cast<uint8_t*>(data.data()), data.size());
   mlogd("read: nghttp2_session_mem_recv... done ({})", rv);

   if (rv < 0)
   {
      mloge("nghttp2_session_mem_recv: {}", nghttp2_strerror(rv));
      nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
      // throw std::runtime_error("nghttp2_session_mem_recv");
      return;
   }

   assert(rv == data.size());
   m_buffer.consume(rv);
}

//
// The read loop is better suited for implementation as a coroutine than the write loop,
// because it does not need to wait on re-activation by the user.
//
template <typename Stream>
awaitable<void> NGHTttp2SessionImpl<Stream>::recv_loop()
{
   m_buffer.reserve(64 * 1024);

   std::string reason("done");
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto free = m_buffer.capacity() - m_buffer.size();
      auto [ec, n] = co_await m_stream.async_read_some(m_buffer.prepare(free), as_tuple(deferred));
      if (ec)
      {
         mlogd("read: {}, terminating session", ec.message());
         reason = ec.message();
         nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
         break;
      }
      m_buffer.commit(n);

      handle_buffer_contents();
      start_write();
   }

   mlogi("recv loop: {}", reason);
   if (reason == "done")
      nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
   start_write();
   mlogi("recv loop: {}, served {} requests", reason, m_requestCounter);
}

// =================================================================================================

template awaitable<void> NGHTttp2SessionImpl<asio::ip::tcp::socket>::recv_loop();
template awaitable<void> NGHTttp2SessionImpl<asio::ip::tcp::socket>::send_loop();

template awaitable<void> NGHTttp2SessionImpl<asio::ssl::stream<asio::ip::tcp::socket>>::recv_loop();
template awaitable<void> NGHTttp2SessionImpl<asio::ssl::stream<asio::ip::tcp::socket>>::send_loop();

} // namespace anyhttp::nghttp2
