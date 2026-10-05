#include "anyhttp/h2/session.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h2/common.hpp"
#include "anyhttp/h2/session_details.hpp" // IWYU pragma: keep
#include "anyhttp/h2/stream.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <boost/asio/buffer.hpp>

#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/impl/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>

#include <boost/container/small_vector.hpp>

#include <boost/url/format.hpp>
#include <boost/url/parse.hpp>

#include <nghttp2/nghttp2.h>

#include <charconv>
#include <string>

namespace http = boost::beast::http;

// =================================================================================================

namespace anyhttp::nghttp2
{

static std::string_view frame_type(uint8_t type)
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

static int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);

   if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
      return 0;

   handler->create_stream(frame->hd.stream_id);
   return 0;
}

//
// TODO: there is on_header_callback2, which can help in avoiding copying strings
//
static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
                              const uint8_t* name_, size_t namelen_, const uint8_t* value_,
                              size_t valuelen_, uint8_t flags, void* user_data)
{
   std::ignore = session;
   std::ignore = flags;

   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto name = make_string_view(name_, namelen_);
   auto value = make_string_view(value_, valuelen_);

   auto stream = handler->find_stream(frame->hd.stream_id);
   assert(stream);

   //
   // Beyond the limit, fields are not stored any more, but nghttp2 still has to decode them: HPACK
   // state is shared by the whole connection. What happens to the stream is decided once the
   // header block is complete, see NGHttp2Stream::on_request() and on_response().
   //
   if (stream->header_limit_exceeded)
      return 0;

   stream->header_size += header_field_size(name, value);
   if (stream->header_size > handler->max_header_size_)
   {
      logw("[{}] header section exceeds {} bytes, ignoring the rest", handler->log_prefix(frame),
           handler->max_header_size_);
      stream->header_limit_exceeded = true;
      stream->received_headers.clear();
      return 0;
   }

   //
   // Headers are logged as a block, after the request or status line, see on_frame_recv_callback().
   //
   if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))
      stream->received_headers.emplace_back(name, value);

   try
   {
      if (name == ":method")
         stream->method = value;
      else if (name == ":path")
      {
         if (auto url = boost::urls::parse_relative_ref(value); url.has_value())
         {
            stream->url.set_path(url->path());
            if (url->has_query())
               stream->url.set_query(url->query());
            if (url->has_fragment())
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

      if (!name.starts_with(':'))
         stream->fields.insert(name, value);
   }
   catch (std::exception& ex)
   {
      logw("[{}] ignoring invalid header: {} ({})", handler->log_prefix(frame), value, ex.what());
   }

   return 0;
}

static int on_frame_not_send_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                      int lib_error_code, void* user_data)
{
   const auto handler = static_cast<NGHttp2Session*>(user_data);
   logw("[{}] on_frame_not_send_callback: {} {}", handler->log_prefix(frame),
        frame_type(frame->hd.type), nghttp2_strerror(lib_error_code));

   //
   // nghttp2 closes the stream of a request HEADERS frame that could not be sent, but leaves the
   // stream of a response open -- with neither side ever learning that there will be no response.
   // Resetting it tells the peer and closes the stream here, too.
   //
   if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat != NGHTTP2_HCAT_REQUEST)
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                NGHTTP2_INTERNAL_ERROR);

   return 0;
}

static int on_error_callback(nghttp2_session* session, int lib_error_code, const char* msg,
                             size_t len, void* user_data)
{
   std::ignore = session;
   std::ignore = lib_error_code;
   auto handler = static_cast<NGHttp2Session*>(user_data);
   loge("[{}] on_error_callback: {}", handler->log_prefix(), std::string_view(msg, len));
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

static int on_invalid_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                      nghttp2_rcbuf* name, nghttp2_rcbuf* value, uint8_t flags,
                                      void* user_data)
{
   std::ignore = session;
   std::ignore = frame;
   std::ignore = flags;
   auto handler = static_cast<NGHttp2Session*>(user_data);
   loge("[{}] invalid_header_callback: {}: {}", //
        handler->log_prefix(), to_string_view(name), to_string_view(value));
   return 0;
}

static int on_invalid_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                          int lib_error_code, void* user_data)
{
   std::ignore = session;
   const auto handler = static_cast<NGHttp2Session*>(user_data);
   logw("[{}] on_invalid_frame_recv_callback: {} {}", handler->log_prefix(frame),
        frame_type(frame->hd.type), nghttp2_strerror(lib_error_code));
   return 0;
}

/**
 * This generic callback is invoked after the more specific ones, e.g. on_header_callback().
 */
static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                  void* user_data)
{
   const auto handler = static_cast<NGHttp2Session*>(user_data);

   //
   // ALTSVC is an extension frame, and only ever received when it has been enabled for the
   // session -- which the client does and the server doesn't, see ClientSession::do_session(). It
   // may arrive on stream 0, carrying the origin it is about, or on a request stream, where the
   // origin is that of the request. Either way it is handled before the stream is looked up: an
   // ALTSVC for a stream that is already gone is to be ignored (RFC 7838, section 4), not
   // answered with the RST_STREAM below.
   //
   if (frame->hd.type == NGHTTP2_ALTSVC)
   {
      const auto* altsvc = static_cast<const nghttp2_ext_altsvc*>(frame->ext.payload);
      const auto value = make_string_view(altsvc->field_value, altsvc->field_value_len);
      logd("[{}] on_frame_recv_callback: ALTSVC: {}", handler->log_prefix(frame), value);
      handler->on_alt_svc(value);
      return 0;
   }

   const auto stream = handler->find_stream(frame->hd.stream_id);

   if (!stream && frame->hd.stream_id > 0)
   {
      logw("[{}] on_frame_recv_callback: {}, but no stream found (id={})", handler->log_prefix(),
           frame_type(frame->hd.type), frame->hd.stream_id);

      // fixes h2spec http/5.1/7
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                NGHTTP2_STREAM_CLOSED);
      return 0;
   }

   switch (frame->hd.type)
   {
   case NGHTTP2_DATA:
      assert(stream);
      logd("[{}] on_frame_recv_callback: DATA len={} flags={}", handler->log_prefix(frame),
           frame->hd.length, frame->hd.flags);

      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
         stream->on_eof(session, frame->hd.stream_id);

      break;

   case NGHTTP2_HEADERS:
   {
      assert(stream);
      using namespace boost::beast::http;
      if (frame->headers.cat == NGHTTP2_HCAT_REQUEST)
         logd("[{}] {} {}", stream->log_prefix_, stream->method, stream->url.buffer());
      else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE && stream->status_code)
         logd("[{}] {} {}", stream->log_prefix_, *stream->status_code,
              obsolete_reason(int_to_status(*stream->status_code)));
      stream->log_received_headers();

      if (frame->headers.cat == NGHTTP2_HCAT_REQUEST)
         stream->on_request();
      else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE)
      {
         //
         // A response may carry an alternative service as a header field instead of, or as well
         // as, in an ALTSVC frame -- the two say the same thing in the same syntax.
         //
         if (auto alt_svc = stream->fields["alt-svc"]; !alt_svc.empty())
            handler->on_alt_svc(std::string_view(alt_svc));

         stream->on_response();
      }

      // end of of stream already? --> no body
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
         stream->on_eof(session, frame->hd.stream_id);

      handler->start_write();
      break;
   }

   case NGHTTP2_WINDOW_UPDATE:
      logd("[{}] on_frame_recv_callback: WINDOW_UPDATE, increment={}", handler->log_prefix(frame),
           frame->window_update.window_size_increment);
      break;

   case NGHTTP2_GOAWAY:
      //
      // Nothing to do here: the streams the GOAWAY leaves open run to completion, and once none
      // is left, nghttp2 wants neither to read nor to write, which ends the session after sending
      // our own GOAWAY (h2spec generic/3.8). Shutting the socket down right away, as we used to,
      // cut those streams off and made that GOAWAY fail with "Broken pipe".
      //
      logd("[{}] on_frame_recv_callback: GOAWAY", handler->log_prefix(frame));
      break;

   default:
      logd("[{}] on_frame_recv_callback: {}", handler->log_prefix(frame),
           frame_type(frame->hd.type));
      break;
   }

   return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id,
                                       const uint8_t* data, size_t len, void* user_data)
{
   std::ignore = flags;

   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = handler->find_stream(stream_id);

   if (!stream)
   {
      logw("[{}.{}] on_data_chunk_recv_callback: DATA, but no stream found (id={})",
           handler->log_prefix(), stream_id, stream_id);
      return 0;
   }

   logd("[{}.{}] on_data_chunk_recv_callback: DATA, len={}", handler->log_prefix(), stream_id, len);
   stream->on_data(session, stream_id, data, len);
   handler->start_write(); // might re-open windows

   return 0;
}

static int on_frame_send_callback(nghttp2_session* session, const nghttp2_frame* frame,
                                  void* user_data)
{
   std::ignore = session;
   std::ignore = frame;

   auto type = frame_type(frame->hd.type);

   auto handler = static_cast<NGHttp2Session*>(user_data);
   if (frame->hd.stream_id)
      logd("[{}] on_frame_send_callback: {} length={} flags={}", handler->log_prefix(frame),
           frame_type(frame->hd.type), frame->hd.length, frame->hd.flags);
   else
      logd("[{}] on_frame_send_callback: {}", handler->log_prefix(), frame_type(frame->hd.type));

   return 0;
}

static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id,
                                    uint32_t error_code, void* user_data)
{
   bool local_close = nghttp2_session_get_stream_local_close(session, stream_id);
   bool remote_close = nghttp2_session_get_stream_remote_close(session, stream_id);

   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_stream_close_callback: {} ({}) (local={}, remote={})",
        handler->log_prefix(stream_id), nghttp2_http2_strerror(error_code), error_code, local_close,
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

NGHttp2Session::NGHttp2Session(std::string_view prefix, Executor executor)
   : executor_(std::move(executor)), log_prefix_(prefix)
{
   mlogd("session created");
}

NGHttp2Session::~NGHttp2Session()
{
   streams_.clear();
   mlogd("streams deleted");
   nghttp2_session_del(session);
   mlogd("session destroyed");
}

// =================================================================================================

void NGHttp2Session::async_submit(SubmitHandler&& handler, std::string_view method,
                                  boost::urls::url url, const Fields& headers)
{
   mlogi("submit: {}", url.buffer());

   if (!session)
   {
      mloge("submit: session already gone!");
      std::move(handler)(errors::canceled, client::Request{nullptr});
      return;
   }
   if (!nghttp2_session_check_request_allowed(session))
   {
      mloge("submit: request not allowed!");
      std::move(handler)(errors::canceled, client::Request{nullptr});
      return;
   }

   auto stream = std::make_shared<NGHttp2Stream>(*this, 0);
   stream->url = url;

   //
   // Submit request, full headers and producer callback for the body.
   //
   // TODO: CONNECT
   //       https://datatracker.ietf.org/doc/html/rfc7540#section-8.3
   //
   std::string method_str(method);
   std::string scheme(url.scheme());
   std::string target(url.encoded_target());
   std::string authority(url.encoded_host_and_port());

   auto nva = boost::container::small_vector<nghttp2_nv, 16>();
   nva.reserve(4 + std::distance(headers.begin(), headers.end()));
   nva.push_back(make_nv_ls(":method", method_str));
   nva.push_back(make_nv_ls(":scheme", scheme));
   nva.push_back(make_nv_ls(":path", target));
   nva.push_back(make_nv_ls(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}': setting pseudo headers is not allowed",
              stream->log_prefix_, item.name_string());

      nva.push_back(make_nv_ls(item.name_string(), item.value()));
   }

   logd("[{}] {} {}", stream->log_prefix_, method_str, url.buffer());
   for (auto nv : nva)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", stream->log_prefix_, truncated(name_of(nv)),
           truncated(value_of(nv)));

   //
   // https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
   //
   // This callback is invoked by nghttp2 when it is ready to accept more data to be sent.
   //
   nghttp2_data_provider2 prd;
   prd.source.ptr = stream.get();
   prd.read_callback = [](nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t {
      std::ignore = session;
      auto stream = static_cast<NGHttp2Stream*>(source->ptr);
      assert(stream);
      assert(stream->id == stream_id);
      return stream->producer_callback(buf, length, data_flags);
   };

   //
   // finally, submit request
   //
   auto id = nghttp2_submit_request2(session, nullptr, nva.data(), nva.size(), &prd, this);
   if (id < 0)
   {
      mloge("submit: nghttp2_submit_request: ERROR: {}", id);
      using namespace boost::system;
      std::move(handler)(make_error_code(errc::invalid_argument), client::Request{nullptr});
   }

   stream->id = id;
   stream->log_prefix_ = std::format("{}.{}", log_prefix(), id);
   last_id_ = id;

   logd("[{}] submit: new stream ID: {}", stream->log_prefix_, id);
   streams_.emplace(id, stream);
   complete_later(std::move(handler), get_executor(), error_code{},
                  client::Request{std::make_unique<NGHttp2Writer<client::Request::Impl>>(*stream)});
   start_write();
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Session::handle_buffer_contents()
{
   mlogd("");
   mlogd("read: nghttp2_session_mem_recv2... ({} bytes)", buffer_.size());
   auto data = buffer_.data();
   ssize_t rv = nghttp2_session_mem_recv2(session, static_cast<uint8_t*>(data.data()), data.size());
   mlogd("read: nghttp2_session_mem_recv2... done ({})", rv);

   if (rv < 0)
   {
      mloge("nghttp2_session_mem_recv: {}", nghttp2_strerror(rv));
      nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
      // throw std::runtime_error("nghttp2_session_mem_recv");
      return;
   }

   assert(rv == data.size());
   buffer_.consume(rv);
   buffer_.clear();
}

// =================================================================================================

NGHttp2Stream* NGHttp2Session::create_stream(int32_t stream_id)
{
   auto [it, inserted] =
      streams_.emplace(stream_id, std::make_shared<NGHttp2Stream>(*this, stream_id));
   assert(inserted);
   request_counter_++;
   return it->second.get();
}

NGHttp2Stream* NGHttp2Session::find_stream(int32_t stream_id)
{
   if (auto it = streams_.find(stream_id); it != std::end(streams_))
      return it->second.get();
   else
      return nullptr;
}

void NGHttp2Session::delete_stream(int32_t stream_id) { streams_.erase(stream_id); }

void NGHttp2Session::close_stream(int32_t stream_id)
{
   auto it = streams_.find(stream_id);
   if (it == std::end(streams_))
   {
      logd("[{}] close_stream: stream already gone", log_prefix(stream_id));
      return;
   }

   std::shared_ptr<NGHttp2Stream> stream = it->second;
   stream->closed = true;

   //
   // Cancel pending write.
   //
   if (stream->write_handler)
   {
      // Use the same error code as reported by the underlying TCP connection used in HTTP/1.1.
      swap_and_invoke(stream->write_handler, make_error_code(errc::connection_reset));
   }

   //
   // If the stream is closed before the user requests the response, we might have to
   // delay the deletion of the stream until the user does.
   //
   if (!stream->response_delivered)
   {
      //
      // If we have seen a response from the peer, the user could still request it and any buffered
      // data. This may also happen during normal operation, if the server delivers a response
      // before the client calls async_get_response().
      //
      if (stream->has_response || stream->response_error)
      {
         logd("[{}] close_stream: response not delivered yet", log_prefix(stream_id));
         it->second->call_read_handler(); // FIXME: this seems to be not needed
         return; // keep stream for now
      }
   }

   //
   // A body that ended cleanly may not have been read to its end yet: the reader has not come back
   // for the rest, which the stream holds. Keep the stream until it has, see
   // NGHttp2Stream::finish_deferred_close(). (A reader that is resumed inline from within the read
   // callback usually keeps up, so this is what posting the completion makes common.)
   //
   if (stream->reader && stream->eof_received && !stream->reading_finished())
   {
      logd("[{}] close_stream: body not read to its end yet", log_prefix(stream_id));
      stream->close_deferred = true;
      return; // keep stream for now
   }

   //
   // Finally, erase stream from map.
   //
   streams_.erase(it);

   logd("[{}] close_stream: found {}, {} streams left", log_prefix(stream_id), (void*)stream.get(),
        streams_.size());

   //
   // This callback is invoked in two situations:
   // 1) We receive a RST frame from the peer
   // 2) We submit a RST frame ourselves
   // In both situations, this stream is deleted. It seems that this may happen multiple times...
   //
   if (stream->read_handler_)
   {
      logd("[{}] stream closed while reading, raising 'partial_message'", log_prefix(stream_id));
      swap_and_invoke(stream->read_handler_, errors::partial_message, 0);
   }

   if (stream->response_handler)
   {
      auto ec = http::make_error_code(http::error::end_of_stream); // be compatible with beast
      //   ec = errc::make_error_code(errc::connection_reset);
      swap_and_invoke(stream->response_handler, ec, client::Response{nullptr});
   }

   //
   // FIXME: We can't just terminate the session after the last request -- what if the user wants
   //        to do another one? Shutting down a session has to be (somewhat) explicit. Try to tie
   //        this to the lifetime of the user-facing 'Session' object...
   //
   // FIXME: Use virtual function instead of dynamic cast for the client-specific code.
   // FIXME: Or even better, use static polymorphism.
   //
   if (auto client = dynamic_cast<ClientReference*>(this) && streams_.empty())
   {
      // nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
      mlogi("last stream closed (id={}), submitting GOAWAY (last stream ID: {})...", stream_id,
            last_id_);
      nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, last_id_, NGHTTP2_NO_ERROR, nullptr, 0);
   }

   // see NGHttp2Stream::call_read_handler() why this is needed
   if (stream)
      run_later(get_executor(), [stream = std::move(stream)]() { /* deferred delete */ });
}

void NGHttp2Session::start_write()
{
   mlogd("start_write: signalling write loop...");
   send_ready_.set();
   mlogd("start_write: signalling write loop... done");
}

// =================================================================================================
// Factories, see anyhttp/h2/backend.hpp. Instantiating the session templates is kept to this
// translation unit, so that the generic server and client never see an nghttp2 type.
// =================================================================================================

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_server_session(server::Server::Impl& server, Executor executor,
                                                   Stream&& stream, std::optional<Upgrade> upgrade)
{
   auto session =
      std::make_shared<ServerSession<Stream>>(server, std::move(executor), std::move(stream));
   session->upgrade_ = std::move(upgrade);
   return session;
}

template <SocketStream Stream>
std::shared_ptr<Session::Impl> make_client_session(client::Client::Impl& client, Executor executor,
                                                   Stream&& stream)
{
   return std::make_shared<ClientSession<Stream>>(client, std::move(executor), std::move(stream));
}

#define ANYHTTP_H2_SERVER(Stream)                                                                  \
   template std::shared_ptr<Session::Impl> make_server_session<Stream>(                            \
      server::Server::Impl&, Executor, Stream&&, std::optional<Upgrade>);
#define ANYHTTP_H2_CLIENT(Stream)                                                                  \
   template std::shared_ptr<Session::Impl> make_client_session<Stream>(client::Client::Impl&,      \
                                                                       Executor, Stream&&);
ANYHTTP_SERVER_STREAMS(ANYHTTP_H2_SERVER)
ANYHTTP_CLIENT_STREAMS(ANYHTTP_H2_CLIENT)
#undef ANYHTTP_H2_SERVER
#undef ANYHTTP_H2_CLIENT

// =================================================================================================

} // namespace anyhttp::nghttp2
