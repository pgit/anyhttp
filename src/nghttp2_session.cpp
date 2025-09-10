#include "anyhttp/nghttp2_session.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/detail/nghttp2_session_details.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/nghttp2_common.hpp"
#include "anyhttp/nghttp2_stream.hpp"

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>
#include <boost/url/parse.hpp>

#include <nghttp2/nghttp2.h>

#include <charconv>
#include <string>

using namespace boost::asio::experimental::awaitable_operators;

// =================================================================================================

namespace anyhttp::nghttp2
{

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
      return "RST_STREAM";
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
      return "CONTINUATION";
   case NGHTTP2_ALTSVC:
      return "ALTSVC";
   case NGHTTP2_ORIGIN:
      return "ORIGIN";
   case NGHTTP2_PRIORITY_UPDATE:
      return "PRIORITY_UPDATE";
   default:
      return "UNKNOWN";
   }
}

// =================================================================================================

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);

   logd("[{}] on_begin_header_callback:", handler->logPrefix(frame));

   if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
      return 0;

   handler->create_stream(frame->hd.stream_id);
   return 0;
}

//
// TODO: there is on_header_callback2, which can help in avoiding copying strings
//
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name_,
                       size_t namelen_, const uint8_t* value_, size_t valuelen_, uint8_t flags,
                       void* user_data)
{
   std::ignore = session;
   std::ignore = flags;

   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto name = make_string_view(name_, namelen_);
   auto value = make_string_view(value_, valuelen_);
   logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", handler->logPrefix(frame), name, value);

   auto stream = handler->find_stream(frame->hd.stream_id);
   assert(stream);

   try
   {
      if (name == ":method")
         stream->method = value;
      else if (name == ":path")
      {
         if (auto url = boost::urls::parse_relative_ref(value); url.has_value())
         {
            stream->url.set_path(url->path());
            stream->url.set_query(url->query());
            stream->url.set_fragment(url->fragment());
         }
      }
      else if (name == ":scheme")
         stream->url.set_scheme(value);
      else if (name == ":authority")
      {
         stream->url.set_encoded_authority(value);
      }
      else if (name == ":host")
      {
         stream->url.set_host(value);
      }
      else if (name == ":status")
      {
         stream->status_code.emplace();
         std::from_chars(value.begin(), value.end(), *stream->status_code);
      }
      else if (name == "content-length")
      {
         stream->content_length.emplace();
         std::from_chars(value.begin(), value.end(), *stream->content_length);
      }
   }
   catch (std::exception& ex)
   {
      logw("[{}] ignoring invalid header: {} ({})", handler->logPrefix(frame), value, ex.what());
   }

   return 0;
}

int on_frame_not_send_callback(nghttp2_session* session, const nghttp2_frame* frame,
                               int lib_error_code, void* user_data)
{
   const auto handler = static_cast<NGHttp2Session*>(user_data);
   logw("[{}] on_frame_not_send_callback: {} {}", handler->logPrefix(frame),
        frameType(frame->hd.type), nghttp2_strerror(lib_error_code));

   return 0;
}

int on_error_callback(nghttp2_session* session, int lib_error_code, const char* msg, size_t len,
                      void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   loge("[{}] on_error_callback: {}", handler->logPrefix(), std::string_view(msg, len));
   return 0;
}

static std::string_view to_string_view(nghttp2_vec vec)
{
   return std::string_view(reinterpret_cast<const char*>(vec.base), vec.len);
}

static std::string_view to_string_view(nghttp2_rcbuf* buf)
{
   return to_string_view(nghttp2_rcbuf_get_buf(buf));
}

int on_invalid_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
                               nghttp2_rcbuf* name, nghttp2_rcbuf* value, uint8_t flags,
                               void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto nameBuf = nghttp2_rcbuf_get_buf(name);
   loge("[{}] invalid_header_callback: {}: {}", //
        handler->logPrefix(), to_string_view(name), to_string_view(value));
   return 0;
}

int on_invalid_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                   int lib_error_code, void* user_data)
{
   const auto handler = static_cast<NGHttp2Session*>(user_data);
   logw("[{}] on_invalid_frame_recv_callback: {} {}", handler->logPrefix(frame),
        frameType(frame->hd.type), nghttp2_strerror(lib_error_code));
   return 0;
}

/**
 * This generic callback is invoked after the more specific ones, e.g. on_header_callback().
 */
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
   const auto handler = static_cast<NGHttp2Session*>(user_data);
   const auto stream = handler->find_stream(frame->hd.stream_id);

   if (!stream && frame->hd.stream_id > 0)
   {
      logw("[{}] on_frame_recv_callback: {}, but no stream found (id={})", handler->logPrefix(),
           frameType(frame->hd.type), frame->hd.stream_id);

      // fixes h2spec http/5.1/7
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                NGHTTP2_STREAM_CLOSED);
      return 0;
   }

   switch (frame->hd.type)
   {
   case NGHTTP2_DATA:
      assert(stream);
      logd("[{}] on_frame_recv_callback: DATA len={} flags={}", handler->logPrefix(frame),
           frame->hd.length, frame->hd.flags);

      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
         stream->on_eof(session, frame->hd.stream_id);

      break;

   case NGHTTP2_HEADERS:
   {
      assert(stream);
      if (frame->headers.cat == NGHTTP2_HCAT_REQUEST)
         stream->on_request();
      else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE)
         stream->on_response();

      // no body?
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
         stream->on_eof(session, frame->hd.stream_id);

      handler->start_write();
      break;
   }

   case NGHTTP2_WINDOW_UPDATE:
      logd("[{}] on_frame_recv_callback: WINDOW_UPDATE, increment={}", handler->logPrefix(frame),
           frame->window_update.window_size_increment);
      break;

   case NGHTTP2_GOAWAY:
      handler->destroy(nullptr); // fixes h2spec generic/3.8
      break;

   default:
      logd("[{}] on_frame_recv_callback: {}", handler->logPrefix(frame), frameType(frame->hd.type));
      break;
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

   logd("[{}.{}] on_data_chunk_recv_callback: DATA, len={}", handler->logPrefix(), stream_id, len);
   stream->on_data(session, stream_id, data, len);
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
      logd("[{}] on_frame_send_callback: {} length={} flags={}", handler->logPrefix(frame),
           frameType(frame->hd.type), frame->hd.length, frame->hd.flags);
   else
      logd("[{}] on_frame_send_callback: {}", handler->logPrefix(), frameType(frame->hd.type));

   return 0;
}

int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code,
                             void* user_data)
{
   bool local_close = nghttp2_session_get_stream_local_close(session, stream_id);
   bool remote_close = nghttp2_session_get_stream_remote_close(session, stream_id);

   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_stream_close_callback: {} ({}) (local={}, remote={})",
        handler->logPrefix(stream_id), nghttp2_http2_strerror(error_code), error_code, local_close,
        remote_close);

   handler->close_stream(stream_id);
   return 0;
}

// =================================================================================================

nghttp2_unique_ptr<nghttp2_session_callbacks> NGHttp2Session::setup_callbacks()
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
   auto callbacks = nghttp2_session_callbacks_new();

   //
   // https://nghttp2.org/documentation/nghttp2_session_server_new.html
   //
   // At a minimum, send and receive callbacks need to be specified.
   //
   auto cbs = callbacks.get();
   // clang-format off
   nghttp2_session_callbacks_set_on_frame_recv_callback     (cbs, on_frame_recv_callback);
   nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_callback);
   nghttp2_session_callbacks_set_on_frame_send_callback     (cbs, on_frame_send_callback);
   nghttp2_session_callbacks_set_on_stream_close_callback   (cbs, on_stream_close_callback);
   nghttp2_session_callbacks_set_on_begin_headers_callback  (cbs, on_begin_headers_callback);
   nghttp2_session_callbacks_set_on_header_callback         (cbs, on_header_callback);
   nghttp2_session_callbacks_set_on_frame_not_send_callback (cbs, on_frame_not_send_callback);
   nghttp2_session_callbacks_set_error_callback2            (cbs, on_error_callback);
   nghttp2_session_callbacks_set_on_invalid_header_callback2(cbs, on_invalid_header_callback);
   // clang-format on
   return callbacks;
}

// =================================================================================================

NGHttp2Session::NGHttp2Session(std::string_view prefix, any_io_executor executor)
   : m_executor(std::move(executor)), m_logPrefix(prefix)
{
   mlogd("session created");
}

NGHttp2Session::~NGHttp2Session()
{
   m_streams.clear();
   mlogd("streams deleted");
   nghttp2_session_del(session);
   mlogd("session destroyed");
}

// =================================================================================================

void NGHttp2Session::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                  const Fields& headers)
{
   mlogi("submit: {}", url.buffer());

   if (!session)
   {
      mloge("submit: session already gone!");
      std::move(handler)(errc::make_error_code(errc::operation_canceled), client::Request{nullptr});
      return;
   }

   auto stream = std::make_shared<NGHttp2Stream>(*this, 0);
   stream->url = url;

   //
   // Submit request, full headers and producer callback for the body.
   //
   std::string method("POST");
   std::string scheme(url.scheme());
   std::string path(url.path());
   std::string authority(url.host_address());
   auto nva = std::vector<nghttp2_nv>();
   // nva.reserve(4 + headers.size());
   nva.push_back(make_nv_ls(":method", method));
   nva.push_back(make_nv_ls(":scheme", scheme));
   nva.push_back(make_nv_ls(":path", path));
   nva.push_back(make_nv_ls(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}'", stream->logPrefix, item.name_string());

      logi("[{}] async_submit: {}: {}", stream->logPrefix, item.name_string(), item.value());
      nva.push_back(make_nv_ls(item.name_string(), item.value()));
   }

   for (auto nv : nva)
      mlogd("submit: {}", nv);

   //
   // https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
   //
   // This callback is invoked by nghttp2 when it is ready to accept more data to be sent.
   //
   nghttp2_data_provider prd;
   prd.source.ptr = stream.get();
   prd.read_callback = [](nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t
   {
      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
      assert(stream);
      assert(stream->id == stream_id);
      return stream->producer_callback(buf, length, data_flags);
   };

   //
   // finally, submit request
   //
   auto id = nghttp2_submit_request(session, nullptr, nva.data(), nva.size(), &prd, this);
   if (id < 0)
   {
      mloge("submit: nghttp2_submit_request: ERROR: {}", id);
      using namespace boost::system;
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
   }

   stream->id = id;
   stream->logPrefix = std::format("{}.{}", logPrefix(), id);
   m_last_id = id;

   logd("submit: stream={}", id);
   m_streams.emplace(id, stream);
   auto writer = std::make_unique<NGHttp2Writer<client::Request::Impl>>(*stream);
   std::move(handler)(boost::system::error_code{}, client::Request{std::move(writer)});
   start_write();
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
   m_buffer.clear();
}

// =================================================================================================

NGHttp2Stream* NGHttp2Session::create_stream(int stream_id)
{
   auto [it, inserted] =
      m_streams.emplace(stream_id, std::make_shared<NGHttp2Stream>(*this, stream_id));
   assert(inserted);
   m_requestCounter++;
   return it->second.get();
}

NGHttp2Stream* NGHttp2Session::find_stream(int32_t stream_id)
{
   if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
      return it->second.get();
   else
      return nullptr;
}

void NGHttp2Session::delete_stream(int32_t stream_id) { m_streams.erase(stream_id); }

void NGHttp2Session::close_stream(int32_t stream_id)
{
   auto it = m_streams.find(stream_id);
   if (it == std::end(m_streams))
   {
      logd("[{}] close_stream: stream already gone", logPrefix(stream_id));
      return;
   }

   std::shared_ptr<NGHttp2Stream> stream = it->second;
   stream->closed = true;

   //
   // Cancel pending send.
   //
   if (stream->write_handler)
   {
      using namespace boost::system;
      swap_and_invoke(stream->write_handler, asio::error::basic_errors::connection_aborted);
   }

   //
   // If the stream is closed before the response has been requested by the user, we might have
   // to delay the deletion of the stream until the user does.
   //
   if (!stream->response_delivered)
   {
      //
      // If we have seen a response from the peer, the user could still request it. This may
      // also happen during normal operation, if the server delivers a response before the
      // client calls async_get_response().
      //
      if (stream->has_response)
      {
         logd("[{}] close_stream: response not delivered yet", logPrefix(stream_id));
         it->second->call_read_handler();
         return;
      }
   }

   //
   // Finally, erase stream from our map.
   //
   m_streams.erase(it);

   logd("[{}] close_stream: found {}, {} streams left", logPrefix(stream_id), (void*)stream.get(),
        m_streams.size());

   //
   // This callback is invoked in two situations:
   // 1) We receive a RST frame from the peer
   // 2) We submit a RST frame ourselves
   // In both situations, this stream is deleted. It seems that this may happen multiple times...
   //
   assert(stream);

   if (stream->m_read_handler)
   {
      logd("[{}] stream closed while reading, raising 'partial_message'", logPrefix(stream_id));
      swap_and_invoke(stream->m_read_handler, boost::beast::http::error::partial_message, 0);
   }
   if (stream->response_handler)
   {
      using namespace boost::system;
      swap_and_invoke(stream->response_handler, errc::make_error_code(errc::io_error), // FIXME:
                      client::Response{nullptr});
   }

   //
   // FIXME: We can't just terminate the session after the last request -- what if the user wants
   //        to do another one? Shutting down a session has to be (somewhat) explicit. Try to tie
   //        this to the lifetime of the user-facing 'Session' object...
   //
   // FIXME: Use virtual function instead of dynamic cast for the client-specific code.
   // FIXME: Or even better, use static polymorphism.
   //
   if (auto client = dynamic_cast<ClientReference*>(this) && m_streams.empty())
   {
      // nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
      logi("[{}] last stream closed (id={}), submitting GOAWAY (last stream ID: {})...",
           m_logPrefix, stream_id, m_last_id);
      nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, m_last_id, NGHTTP2_NO_ERROR, nullptr, 0);
   }

   // see NGHttp2Stream::call_read_handler() why this is needed
   if (stream)
      post(executor(), [stream]() { /* deferred delete */ });
}

void NGHttp2Session::start_write()
{
   if (m_send_handler)
   {
      logd("[{}] start_write: signalling write loop...", m_logPrefix);
      swap_and_invoke(m_send_handler);
      logd("[{}] start_write: signalling write loop... done", m_logPrefix);
   }
}

// =================================================================================================

} // namespace anyhttp::nghttp2
