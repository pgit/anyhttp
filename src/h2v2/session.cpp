#include "anyhttp/h2v2/session.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h2v2/common.hpp"
#include "anyhttp/h2v2/session_details.hpp" // IWYU pragma: keep
#include "anyhttp/h2v2/stream.hpp"
#include "anyhttp/literals.hpp"

#include <boost/asio/buffer.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/beast/http/impl/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>

#include <boost/url/parse.hpp>

#include <nghttp2v2/nghttp2.h>

#include <algorithm>
#include <charconv>
#include <random>
#include <string>

namespace http = boost::beast::http;

// =================================================================================================

namespace anyhttp::nghttp2
{

//
// Version 1 had a frame_type() helper here, for the frame-level callbacks it logged from. Version 2
// has no frame-level callbacks at all: what it reports is what the application is meant to act on,
// which is why there is so much less logging glue below.
//

// =================================================================================================

//
// Mandatory: the library asserts on a missing rand callback. It needs unpredictable data for the
// PING payloads it sends and for its hash table seeds, neither of which is security critical, so
// the default engine is enough -- it is not asked for key material.
//
static void rand_callback(uint8_t* dest, size_t destlen)
{
   static thread_local std::mt19937 engine{std::random_device{}()};
   std::uniform_int_distribution<unsigned int> dist(0, 255);
   std::generate(dest, dest + destlen, [&] { return static_cast<uint8_t>(dist(engine)); });
}

// -------------------------------------------------------------------------------------------------

static int on_stream_open(nghttp2_conn* conn, int64_t stream_id, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = handler->create_stream(stream_id);

   //
   // Every later callback for this stream comes with it as the per-stream user data, so the map
   // lookup version 1 did on each one is needed only where a stream has to be found by id.
   //
   nghttp2_conn_set_stream_user_data(conn, stream_id, stream);
   return 0;
}

static int on_begin_headers(nghttp2_conn*, int64_t, void*, void*) { return 0; }

static int on_recv_header(nghttp2_conn*, int64_t stream_id, int32_t /*token*/, nghttp2_rcbuf* name_,
                          nghttp2_rcbuf* value_, uint8_t /*flags*/, void* user_data,
                          void* stream_user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);
   assert(stream);

   auto name = to_string_view(name_);
   auto value = to_string_view(value_);

   //
   // Beyond the limit, fields are not stored any more, but nghttp2 still has to decode them: HPACK
   // state is shared by the whole connection. What happens to the stream is decided once the
   // field section is complete, see NGHttp2Stream::on_request() and on_response().
   //
   if (stream->header_limit_exceeded)
      return 0;

   stream->header_size += header_field_size(name, value);
   if (stream->header_size > handler->max_header_size_)
   {
      logw("[{}] header section exceeds {} bytes, ignoring the rest",
           handler->log_prefix(stream_id), handler->max_header_size_);
      stream->header_limit_exceeded = true;
      stream->received_headers.clear();
      return 0;
   }

   //
   // Headers are logged as a block, after the request or status line, see on_end_headers().
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
         stream->url.set_encoded_authority(value);
      else if (name == ":host")
         stream->url.set_host(value);
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
      logw("[{}] ignoring invalid header: {} ({})", handler->log_prefix(stream_id), value,
           ex.what());
   }

   return 0;
}

//
// The whole field section has arrived. \p fin says the stream ends with it, but the end of the
// body is not acted on here: end_stream_remote() follows right after, for a section with the flag
// as much as for a DATA frame that carries it, so there is one place where a body ends.
//
static int on_end_headers(nghttp2_conn* conn, int64_t /*stream_id*/, int /*fin*/, void* user_data,
                          void* stream_user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);
   assert(stream);

   using namespace boost::beast::http;
   if (nghttp2_conn_is_server(conn))
      logd("[{}] {} {}", stream->log_prefix(), stream->method, stream->url.buffer());
   else if (stream->status_code)
      logd("[{}] {} {}", stream->log_prefix(), *stream->status_code,
           obsolete_reason(int_to_status(*stream->status_code)));
   stream->log_received_headers();

   if (nghttp2_conn_is_server(conn))
      stream->on_request();
   else
   {
      //
      // A response may carry an alternative service as a header field. Version 1 also accepted the
      // ALTSVC extension frame, which version 2 does not implement.
      //
      if (auto alt_svc = stream->fields["alt-svc"]; !alt_svc.empty())
         handler->on_alt_svc(std::string_view(alt_svc));

      stream->on_response();
   }

   handler->start_write();
   return 0;
}

static int on_recv_data(nghttp2_conn* conn, int64_t stream_id, const uint8_t* data, size_t datalen,
                        void* user_data, void* stream_user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);

   if (!stream)
   {
      logw("[{}] on_recv_data: DATA, but no stream found", handler->log_prefix(stream_id));
      nghttp2_conn_extend_max_offset(conn, datalen);
      return 0;
   }

   logd("[{}] on_recv_data: DATA, len={}", handler->log_prefix(stream_id), datalen);

   //
   // Connection-level credit is given back right away; the stream-level window is only reopened
   // once the reader has taken the data, which is what applies back pressure. Version 1 split the
   // same two steps between nghttp2_session_consume_connection() and consume_stream().
   //
   nghttp2_conn_extend_max_offset(conn, datalen);

   stream->on_data(data, datalen);
   handler->start_write(); // might re-open windows

   return 0;
}

static int on_end_stream_remote(nghttp2_conn*, int64_t stream_id, void* user_data,
                                void* stream_user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);
   if (!stream)
   {
      logd("[{}] on_end_stream_remote: stream already gone", handler->log_prefix(stream_id));
      return 0;
   }

   stream->on_eof();
   return 0;
}

//
// \p len bytes of body starting at \p offset have been copied into a DATA frame, so the buffer
// they came from may be released -- see NGHttp2Stream::data_reader(), which handed them over by
// reference. Version 1 copied in its producer callback and had no need for this.
//
static int on_write_stream_data_offset(nghttp2_conn*, int64_t stream_id, uint64_t /*offset*/,
                                       size_t len, void* user_data, void* stream_user_data)
{
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);
   if (!stream)
   {
      auto handler = static_cast<NGHttp2Session*>(user_data);
      logw("[{}] on_write_stream_data_offset: stream already gone", handler->log_prefix(stream_id));
      return 0;
   }

   stream->on_write_offset(len);
   return 0;
}

static int on_stream_close(nghttp2_conn*, uint32_t flags, int64_t stream_id, uint32_t error_code,
                           void* user_data, void* /*stream_user_data*/)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   if (flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET)
      logd("[{}] on_stream_close: {} ({})", handler->log_prefix(stream_id),
           nghttp2_http2_strerror(error_code), error_code);
   else
      logd("[{}] on_stream_close: clean", handler->log_prefix(stream_id));

   handler->close_stream(stream_id);
   return 0;
}

static int on_recv_settings_ack(nghttp2_conn*, void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_recv_settings_ack:", handler->log_prefix());
   return 0;
}

static int on_shutdown(nghttp2_conn*, int64_t last_stream_id, uint32_t error_code, void* user_data)
{
   //
   // The peer sent a GOAWAY. Nothing to do: the streams it leaves open run to completion, and once
   // none is left nghttp2_conn_write() reports NGHTTP2_ERR_CLOSING, which ends the session after
   // our own GOAWAY has gone out.
   //
   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_shutdown: GOAWAY, last_stream_id={} {}", handler->log_prefix(), last_stream_id,
        nghttp2_http2_strerror(error_code));
   return 0;
}

// =================================================================================================

nghttp2_callbacks NGHttp2Session::setup_callbacks()
{
   //
   // One struct instead of version 1's callbacks object with a setter per entry. There is no send
   // or receive callback to set: nghttp2_conn_read() and nghttp2_conn_write() take the buffers.
   //
   nghttp2_callbacks callbacks{};
   // clang-format off
   callbacks.rand                     = rand_callback;
   callbacks.stream_open              = on_stream_open;
   callbacks.stream_close             = on_stream_close;
   callbacks.begin_headers            = on_begin_headers;
   callbacks.recv_header              = on_recv_header;
   callbacks.end_headers              = on_end_headers;
   callbacks.recv_data                = on_recv_data;
   callbacks.end_stream_remote        = on_end_stream_remote;
   callbacks.write_stream_data_offset = on_write_stream_data_offset;
   callbacks.recv_settings_ack        = on_recv_settings_ack;
   callbacks.shutdown                 = on_shutdown;
   // clang-format on
   return callbacks;
}

void NGHttp2Session::setup_settings(nghttp2_settings& settings)
{
   nghttp2_settings_default(&settings);

   settings.initial_ts = timestamp();

   //
   // The header section limit is the library's own business in version 2 -- version 1 had neither
   // a setting for it nor any enforcement, which is why on_recv_header() still counts the section
   // by hand: that is what turns an oversized one into a 431 instead of a connection error.
   //
   settings.max_field_section_size = max_header_size_;

   settings.max_concurrent_streams_remote = 100;
   settings.initial_max_stream_data = 1_m;
   settings.initial_max_data = 1_m;
}

// =================================================================================================

NGHttp2Session::NGHttp2Session(std::string_view prefix, Executor executor)
   : executor_(std::move(executor)), log_prefix_(prefix), timer_(executor_)
{
   mlogd("session created");
}

NGHttp2Session::~NGHttp2Session()
{
   timer_.cancel();
   streams_.clear();
   mlogd("streams deleted");
   nghttp2_conn_del(conn);
   mlogd("session destroyed");
}

// =================================================================================================

void NGHttp2Session::async_submit(SubmitHandler&& handler, std::string_view /*method*/,
                                  boost::urls::url url, const Fields& /*headers*/)
{
   //
   // Not implemented yet: this slice of the version 2 backend covers the server only, see
   // ClientSession::do_session(). Failing the submit is what the generic client turns into an
   // error on the request it was asked for.
   //
   mloge("submit: HTTP/2 over nghttp2 v2 has no client yet ({})", url.buffer());
   complete_later(std::move(handler), get_executor(), errors::canceled, client::Request{nullptr});
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Session::handle_buffer_contents()
{
   if (closing_)
   {
      buffer_.clear();
      return;
   }

   mlogd("read: nghttp2_conn_read... ({} bytes)", buffer_.size());
   auto data = buffer_.data();
   const auto rv =
      nghttp2_conn_read(conn, static_cast<const uint8_t*>(data.data()), data.size(), timestamp());
   mlogd("read: nghttp2_conn_read... done ({})", rv);

   //
   // Unlike version 1's mem_recv, this consumes all of the input or fails for good: "the error is
   // unrecoverable, therefore the application should close the underlying connection without
   // calling any nghttp2 API". So there is nothing to terminate the connection with -- flagging it
   // as closing is what ends both loops.
   //
   if (rv != 0)
   {
      mloge("nghttp2_conn_read: {}", nghttp2_strerror(rv));
      closing_ = true;
   }

   buffer_.consume(data.size());
   buffer_.clear();
}

// =================================================================================================

NGHttp2Stream* NGHttp2Session::create_stream(int64_t stream_id)
{
   auto [it, inserted] =
      streams_.emplace(stream_id, std::make_shared<NGHttp2Stream>(*this, stream_id));
   assert(inserted);
   last_id_ = stream_id;
   request_counter_++;
   return it->second.get();
}

NGHttp2Stream* NGHttp2Session::find_stream(int64_t stream_id)
{
   if (auto it = streams_.find(stream_id); it != std::end(streams_))
      return it->second.get();
   else
      return nullptr;
}

void NGHttp2Session::delete_stream(int64_t stream_id) { streams_.erase(stream_id); }

void NGHttp2Session::close_stream(int64_t stream_id)
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
   // nghttp2 is about to forget the stream, so the pointer it hands back as per-stream user data
   // has to go with it -- an acknowledgement or a reset arriving after this would otherwise reach
   // a stream we have dropped.
   //
   if (conn)
      nghttp2_conn_set_stream_user_data(conn, stream_id, nullptr);

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
      if (stream->has_response || stream->response_error)
      {
         logd("[{}] close_stream: response not delivered yet", log_prefix(stream_id));
         it->second->call_read_handler();
         return; // keep stream for now
      }
   }

   //
   // A body that ended cleanly may not have been read to its end yet: the reader has not come back
   // for the rest, which the stream holds. Keep the stream until it has, see
   // NGHttp2Stream::finish_deferred_close().
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

   if (stream->read_handler_)
   {
      logd("[{}] stream closed while reading, raising 'partial_message'", log_prefix(stream_id));
      swap_and_invoke(stream->read_handler_, errors::partial_message, 0);
   }

   if (stream->response_handler)
   {
      auto ec = http::make_error_code(http::error::end_of_stream); // be compatible with beast
      swap_and_invoke(stream->response_handler, ec, client::Response{nullptr});
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

//
// The connection timer, the same shape as QuicSession's: nghttp2 says when it wants to be called
// back, and handle_expiry() does the work and gets the send loop going again. Version 1 had no
// such thing -- it kept its SETTINGS timeout on its own clock.
//
void NGHttp2Session::arm_timer_from_nghttp2()
{
   if (closing_ || !conn)
      return;

   const auto expiry = nghttp2_conn_get_expiry(conn);
   if (expiry == UINT64_MAX)
   {
      //
      // Nothing pending. Cancel the current timer so an old one does not fire for no reason, and
      // does not keep the context alive indefinitely.
      //
      timer_.cancel();
      return;
   }

   const auto now = timestamp();
   const auto delay =
      expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};

   timer_.arm(delay, [self = weak_from_this()] {
      if (auto session = std::static_pointer_cast<NGHttp2Session>(self.lock()))
         session->handle_expiry();
   });
}

void NGHttp2Session::handle_expiry()
{
   if (closing_ || !conn)
      return;

   if (auto rv = nghttp2_conn_handle_expiry(conn, timestamp()); rv != 0)
   {
      mlogw("nghttp2_conn_handle_expiry: {}", nghttp2_strerror(rv));
      closing_ = true;
   }

   start_write();
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
