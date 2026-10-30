#include "anyhttp/h2v2/session.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h2v2/common.hpp"
#include "anyhttp/h2v2/session_details.hpp" // IWYU pragma: keep
#include "anyhttp/h2v2/stream.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/tls.hpp"

#include <boost/asio/buffer.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/beast/http/impl/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>

#include <boost/container/small_vector.hpp>

#include <boost/url/parse.hpp>

#include <nghttp2v2/nghttp2.h>

#include <charconv>
#include <chrono>
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

//
// A trailer section. Version 2 reports one through callbacks of its own, where version 1 had the
// single on_header_callback() for every field section there is -- which is how a trailer field
// ended up in \c fields there, after the request or response had long been handed to the user.
// Keeping that, by letting the trailer fields through the same callback, is what makes the two
// backends behave the same; wiring up end_trailers separately is what keeps on_request() and
// on_response() from running a second time.
//
// TODO: neither backend really supports trailers, and nor do h1/ and h3/ -- a trailer section that
//       arrives after its message was delivered is of no use to the reader. Exposing them would be
//       a change to the Reader interface, the same one in all three protocols.
//
static int on_end_trailers(nghttp2_conn*, int64_t /*stream_id*/, int /*fin*/, void* /*user_data*/,
                           void* stream_user_data)
{
   auto stream = static_cast<NGHttp2Stream*>(stream_user_data);
   assert(stream);
   stream->log_received_headers();
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

//
// A PING of ours has been acknowledged. Version 1 had to recognize this in its frame callback and
// time it itself; version 2 reports the round trip time with it. Nothing sends a PING here yet, so
// this only ever fires for one the library sent on its own.
//
static int on_recv_ping_ack(nghttp2_conn*, const nghttp2_ping_data*, nghttp2_duration rtt,
                            void* user_data)
{
   auto handler = static_cast<NGHttp2Session*>(user_data);
   logd("[{}] on_recv_ping_ack: rtt={}", handler->log_prefix(),
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds{rtt}));
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
   // `rand` is the one entry that is not optional -- version 1 needed no randomness, version 2
   // asserts on a missing callback, there being no default to fall back on. tls_random_bytes() has
   // the signature nghttp2_rand declares, so it serves as the callback itself.
   //
   nghttp2_callbacks callbacks{};
   // clang-format off
   callbacks.rand                     = tls_random_bytes;
   callbacks.stream_open              = on_stream_open;
   callbacks.stream_close             = on_stream_close;
   callbacks.begin_headers            = on_begin_headers;
   callbacks.recv_header              = on_recv_header;
   callbacks.end_headers              = on_end_headers;
   callbacks.begin_trailers           = on_begin_headers;
   callbacks.recv_trailer             = on_recv_header;
   callbacks.end_trailers             = on_end_trailers;
   callbacks.recv_data                = on_recv_data;
   callbacks.end_stream_remote        = on_end_stream_remote;
   callbacks.write_stream_data_offset = on_write_stream_data_offset;
   callbacks.recv_settings_ack        = on_recv_settings_ack;
   callbacks.recv_ping_ack            = on_recv_ping_ack;
   callbacks.shutdown                 = on_shutdown;
   // clang-format on
   return callbacks;
}

void NGHttp2Session::setup_settings(nghttp2_settings& settings)
{
   nghttp2_settings_default(&settings);

   settings.initial_ts = timestamp();

   //
   // Version 2 does enforce a header section limit of its own, and resets the stream with
   // PROTOCOL_ERROR the moment a field crosses it -- before end_headers, so before anything here
   // can answer. anyhttp answers an oversized section with 431 (a client's async_get_response()
   // with http::error::header_limit), the same way in HTTP/1.1 and HTTP/3, and that is worth more
   // to a peer than a bare reset. So the library's limit is taken out of the way and
   // on_recv_header() counts the section itself, exactly as in version 1.
   //
   // Nothing is given up by this: the CONTINUATION flood this setting is sometimes credited with
   // stopping is capped separately, by NGHTTP2_MAX_CONTINUATIONS, and a section past our own limit
   // is no longer stored. The default here would be 65536, not "no limit", so it has to be said.
   //
   settings.max_field_section_size = UINT32_MAX;

   settings.max_concurrent_streams_remote = max_concurrent_streams_;
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

void NGHttp2Session::async_submit(SubmitHandler&& handler, std::string_view method,
                                  boost::urls::url url, const Fields& headers)
{
   mlogi("submit: {}", url.buffer());

   if (!conn || closing_)
   {
      mloge("submit: session already gone!");
      std::move(handler)(errors::canceled, client::Request{nullptr});
      return;
   }

   //
   // A request body goes through the stream's own buffer rather than being handed over by
   // reference: a cancelled write has to leave the stream usable, so that the body can still be
   // ended afterwards. See WriteMode.
   //
   auto stream = std::make_shared<NGHttp2Stream>(*this, 0, WriteMode::Staged);
   stream->url = url;

   //
   // Submit request with the full header section; the body is pulled from the data reader.
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
              stream->log_prefix(), item.name_string());

      nva.push_back(make_nv_ls(item.name_string(), item.value()));
   }

   logd("[{}] {} {}", stream->log_prefix(), method_str, url.buffer());
   for (auto nv : nva)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", stream->log_prefix(), truncated(name_of(nv)),
           truncated(value_of(nv)));

   //
   // The stream is created by this call, and the id is only known afterwards -- unlike a request
   // that arrives, which gets its stream in the stream_open callback. Version 1 needed a separate
   // check whether a request was allowed at all; here the submit itself says so.
   //
   auto dr = make_data_reader();
   const auto id = nghttp2_conn_submit_request(conn, nva.data(), nva.size(), &dr, stream.get());
   if (id < 0)
   {
      const auto rv = static_cast<int>(id);

      //
      // The peer's MAX_CONCURRENT_STREAMS is reached. Version 1 queued such a request and sent it
      // once a stream came free; version 2 refuses it, which is the behaviour Session::async_submit
      // describes for a protocol that limits concurrency -- the caller may be the one who has to
      // finish a request first, so waiting here could deadlock.
      //
      error_code ec;
      if (rv == NGHTTP2_ERR_STREAM_ID_BLOCKED)
         ec = errors::would_block;
      else if (rv == NGHTTP2_ERR_REFUSED_STREAM) // a GOAWAY has arrived, no new stream is allowed
         ec = errors::connection_aborted;
      else
         ec = make_error_code(errc::invalid_argument);

      // a request that only has to wait for a free stream is normal back pressure, not a problem
      logwd(ec != errors::would_block, "[{}] submit: nghttp2_conn_submit_request: {}",
            stream->log_prefix(), nghttp2_strerror(rv));
      std::move(handler)(ec, client::Request{nullptr});
      return;
   }

   stream->id = id;
   last_id_ = id;

   logd("[{}] submit: new stream ID: {}", stream->log_prefix(), id);
   streams_.emplace(id, stream);
   complete_later(std::move(handler), get_executor(), error_code{},
                  client::Request{std::make_unique<NGHttp2Writer<client::Request::Impl>>(*stream)});
   start_write();
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
   //
   // A stream the peer opened, so what this side writes on it is a response: handed to nghttp2 by
   // reference, which is what makes serving a large body copy-free. See WriteMode.
   //
   auto [it, inserted] = streams_.emplace(
      stream_id, std::make_shared<NGHttp2Stream>(*this, stream_id, WriteMode::ZeroCopy));
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
   // Cancel pending write. A write that asked for this reset itself, because it was cancelled with
   // bytes nghttp2 was still reading, gets the cancellation it is waiting for -- see
   // NGHttp2Stream::async_write(). Any other one gets the error code the underlying TCP connection
   // would report in HTTP/1.1.
   //
   if (stream->write_handler)
   {
      swap_and_invoke(stream->write_handler, stream->write_cancelled_
                                                ? errors::canceled
                                                : make_error_code(errc::connection_reset));
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

   //
   // Nothing happens here when a client runs out of streams, and nothing should: the session has
   // to stay usable for the next request, which the user may submit at any time. It ends when the
   // peer closes the connection or the user lets go of the session, see destroy().
   //
   // Version 1 has a block here that submits a GOAWAY naming last_id_ once the last stream is
   // gone. That one never reaches the wire: a GOAWAY's last stream ID has to be a stream the peer
   // opened, so nghttp2_submit_goaway() refuses a client's own ID with NGHTTP2_ERR_INVALID_ARGUMENT
   // -- which that code does not look at. Doing it properly here, with nghttp2_conn_shutdown(),
   // ends the session for real, and then a client that submits again a moment later finds it gone.
   //

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
