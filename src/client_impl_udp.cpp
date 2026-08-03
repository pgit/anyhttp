//
// anyhttp QUIC / HTTP/3 client.
//
// One `Http3ClientSession` per QUIC connection implements `Session::Impl`. Unlike the server
// (`server_impl_udp.cpp`), which multiplexes many connections over one shared UDP socket demuxed
// by connection ID, each client session owns its own `connect()`-ed UDP socket -- there is exactly
// one peer, so no demux table is needed. Per-request `Http3ClientStream` state feeds an
// `Http3ClientWriter` (client::Request) and `Http3ClientReader` (client::Response), mirroring the
// server-side Http3Writer/Http3Reader adapters.
//
// Not yet implemented: certificate verification, 0-RTT, connection migration, GSO/ECN, retry
// tokens, graceful (multi-PTO) close.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <array>
#include <charconv>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ngtcp2/shared.h"
#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;
namespace errc = boost::system::errc;

namespace anyhttp::client
{

// =================================================================================================
// Free-standing helpers
// =================================================================================================

namespace
{

//
// One-shot process-wide initialization of ngtcp2_crypto_ossl and the client-role OpenSSL SSL_CTX
// used for every outgoing QUIC connection.
//
struct TlsClientContext
{
   TlsClientContext()
   {
      static const int init_once = []
      {
         if (ngtcp2_crypto_ossl_init() != 0)
            throw std::runtime_error("ngtcp2_crypto_ossl_init");
         return 0;
      }();
      (void)init_once;

      ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new");

      static constexpr unsigned char alpn[] = "\x02h3";
      SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn) - 1);

      //
      // TODO: verify the server certificate (e.g. against pki/out/root.pem) instead of accepting
      // anything. Matches how the plain-TCP h1/h2 client paths currently have no TLS at all, and
      // how the External.curl_https/curl_multiple_https h3 cases use `--cacert` (h1/h2 use `-k`).
      //
      SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
   }

   ~TlsClientContext()
   {
      if (ctx)
         SSL_CTX_free(ctx);
   }

   TlsClientContext(const TlsClientContext&) = delete;
   TlsClientContext& operator=(const TlsClientContext&) = delete;

   SSL_CTX* ctx = nullptr;
};

TlsClientContext& tls_context()
{
   static TlsClientContext instance;
   return instance;
}

// -------------------------------------------------------------------------------------------------

nghttp3_nv make_nv(std::string_view name, std::string_view value)
{
   nghttp3_nv nv{};
   nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
   nv.namelen = name.size();
   nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
   nv.valuelen = value.size();
   nv.flags = NGHTTP3_NV_FLAG_NONE;
   return nv;
}

void ngtcp2_log_printf(void* /*user*/, const char* fmt, ...) noexcept
{
   if (!spdlog::default_logger()->should_log(spdlog::level::trace))
      return;
   std::array<char, 512> buf;
   va_list ap;
   va_start(ap, fmt);
   std::vsnprintf(buf.data(), buf.size(), fmt, ap);
   va_end(ap);
   spdlog::trace("{}", buf.data());
}

} // namespace

// =================================================================================================
// Http3ClientStream: per-request state.
// =================================================================================================

class Http3ClientSession;
class Http3ClientStream;

class Http3ClientStream
{
public:
   Http3ClientStream(Http3ClientSession& session, int64_t id);
   ~Http3ClientStream();

   int64_t id;
   Http3ClientSession& session;
   std::string log_prefix;

   //
   // Request state, set once by async_submit() before headers are sent.
   //
   boost::urls::url url;

   //
   // Response state (populated by nghttp3 header callbacks).
   //
   unsigned int status_code = 0;
   Fields response_fields;
   std::optional<size_t> content_length;
   bool headers_received = false;
   bool response_delivered = false;
   client::Request::GetResponseHandler response_handler;

   //
   // Request body plumbing (client -> server), mirrors the server's Http3Stream.
   //
   std::deque<std::vector<uint8_t>> pending_writes;
   std::vector<std::vector<uint8_t>> in_flight_writes;
   bool eof_submitted = false;
   bool eof_sent_to_h3 = false;

   //
   // Response body plumbing (server -> client).
   //
   std::deque<std::vector<uint8_t>> pending_read;
   asio::const_buffer read_head;
   bool eof_received = false;
   ReadSomeHandler read_handler;
   asio::mutable_buffer read_handler_buffer;

   //
   // Lifecycle.
   //
   impl::Writer* writer = nullptr; // Http3ClientWriter, the client::Request
   impl::Reader* reader = nullptr; // Http3ClientReader, the client::Response
   bool closed = false;

   asio::any_io_executor get_executor() const noexcept;
   const std::string& logPrefix() const noexcept { return log_prefix; }

   // Data flow into user land (response body).
   void on_data_chunk(const uint8_t* data, size_t len);
   void on_eof();
   void call_read_handler();

   // Data flow from user land back to nghttp3 (request body).
   nghttp3_ssize data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags);
   void on_write_consumed(size_t n);

   // async_get_response()
   void async_get_response(client::Request::GetResponseHandler&& handler);
   void deliver_response();

   // Called on abrupt stream close/reset before completion.
   void fail(boost::system::error_code ec);

   // Called from either reader or writer destructor.
   void delete_reader();
   void delete_writer();
   void maybe_close();
};

// =================================================================================================
// Http3ClientSession: one QUIC connection, one anyhttp Session::Impl.
// =================================================================================================

class Http3ClientSession : public Session::Impl
{
public:
   explicit Http3ClientSession(asio::any_io_executor executor);
   ~Http3ClientSession() override;

   //
   // Session::Impl
   //
   asio::any_io_executor get_executor() const noexcept override { return executor_; }
   void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers) override;
   awaitable<void> do_session(Buffer&& data) override;
   void destroy() noexcept override;

   //
   // Connect-time setup. Returns 0 on success.
   //
   int init(asio::ip::udp::endpoint remote);

   //
   // Awaited by client::Client::Impl::async_connect() before handing the Session back to the
   // caller. Fires once the QUIC handshake has progressed far enough to create the HTTP/3 layer
   // (see setup_http3()), or once the connection has failed/closed before getting that far -- in
   // which case `ready()` is still false and the caller should synthesize an error.
   //
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                CompletionToken = DefaultCompletionToken>
   auto wait_ready(CompletionToken&& token = CompletionToken())
   {
      return ready_signal_.async_wait(std::forward<CompletionToken>(token));
   }
   bool ready() const noexcept { return h3_ != nullptr; }

   const std::string& logPrefix() const noexcept { return log_prefix_; }
   nghttp3_conn* h3() const noexcept { return h3_; }

   Http3ClientStream* find_stream(int64_t id);
   Http3ClientStream* create_stream(int64_t id);
   void erase_stream(int64_t id);

   // Called by Http3ClientWriter to make sure the write loop runs after new data was queued.
   void wake_write();

   //
   // ngtcp2 <-> ngtcp2_crypto_ossl bridge.
   //
   static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref)
   {
      return static_cast<Http3ClientSession*>(ref->user_data)->conn_;
   }

   //
   // ngtcp2 callback bridges
   //
   static int cb_handshake_completed(ngtcp2_conn*, void* user);
   static int cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t offset,
                                  const uint8_t* data, size_t datalen, void* user, void*);
   static int cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset,
                                          uint64_t datalen, void* user, void*);
   static int cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                              uint64_t app_error_code, void* user, void*);
   static void cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*);
   static int cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                       void* user);
   static int cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user);
   static int cb_extend_max_local_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user);
   static int cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*);
   static int cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t final_size,
                              uint64_t app_error_code, void* user, void*);
   static int cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t max_data,
                                        void* user, void*);
   static int cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user);

   //
   // nghttp3 callback bridges
   //
   static int h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data, size_t datalen,
                              void* user, void*);
   static int h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed, void* user,
                                     void*);
   static int h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token, nghttp3_rcbuf* name,
                                nghttp3_rcbuf* value, uint8_t flags, void* user, void*);
   static int h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int fin, void* user, void*);
   static int h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);

private:
   int setup_http3();
   int on_read(std::span<const uint8_t> data);
   int write_streams();
   void send_udp(std::span<const uint8_t> data);
   void update_timer();
   void arm_timer_from_ngtcp2();
   int handle_expiry();
   int handle_error(int rv);
   void close();
   void signal_ready();

private:
   asio::any_io_executor executor_;
   asio::ip::udp::socket socket_;

   ngtcp2_conn* conn_ = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr;
   ngtcp2_crypto_conn_ref conn_ref_{};

   nghttp3_conn* h3_ = nullptr;

   asio::steady_timer timer_; // ngtcp2 expiry (handshake / idle / PTO)
   asio::steady_timer ready_signal_; // sentinel timer, see wait_ready()
   ngtcp2_ccerr last_error_{};
   bool closed_ = false;

   std::string log_prefix_;

   std::unordered_map<int64_t, std::shared_ptr<Http3ClientStream>> streams_;
};

// =================================================================================================
// Http3ClientWriter / Http3ClientReader: adapters plugging Http3ClientStream into client::Request
// / client::Response.
// =================================================================================================

class Http3ClientWriter : public client::Request::Impl
{
public:
   explicit Http3ClientWriter(Http3ClientStream& s) : stream(&s) { s.writer = this; }
   ~Http3ClientWriter() override
   {
      if (stream)
      {
         stream->writer = nullptr;
         stream->delete_writer();
      }
   }

   asio::any_io_executor get_executor() const noexcept override
   {
      assert(stream);
      return stream->get_executor();
   }

   void content_length(std::optional<size_t> /*len*/) override
   {
      // Request headers (including any content-length the user set beforehand) are already
      // submitted synchronously in Http3ClientSession::async_submit(); nothing to do here.
   }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(errc::make_error_code(errc::connection_reset));
         return;
      }

      auto n = asio::buffer_size(buffer);
      if (n == 0)
      {
         stream->eof_submitted = true;
      }
      else
      {
         auto* src = static_cast<const uint8_t*>(buffer.data());
         stream->pending_writes.emplace_back(src, src + n);
      }

      if (auto h3 = stream->session.h3())
         nghttp3_conn_resume_stream(h3, stream->id);
      stream->session.wake_write();

      asio::any_completion_executor ex =
         asio::get_associated_immediate_executor(handler, stream->get_executor());
      ex.execute([handler = std::move(handler)]() mutable
      { std::move(handler)(boost::system::error_code{}); });
   }

   //
   // Part of the shared Writer-based interface (mirrors server::Response::Impl), but never
   // actually invoked for a client::Request -- client.hpp does not expose async_submit()
   // publicly. Kept only to satisfy the pure virtual.
   //
   void async_submit(StatusHandler&& handler, unsigned int /*status_code*/,
                     const Fields& /*headers*/) override
   {
      std::move(handler)(boost::system::error_code{});
   }

   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      if (!stream)
      {
         std::move(handler)(errc::make_error_code(errc::connection_aborted), client::Response{nullptr});
         return;
      }
      stream->async_get_response(std::move(handler));
   }

   void detach() override { stream = nullptr; }

   Http3ClientStream* stream;
};

class Http3ClientReader : public client::Response::Impl
{
public:
   explicit Http3ClientReader(Http3ClientStream& s) : stream(&s) { s.reader = this; }
   ~Http3ClientReader() override
   {
      if (stream)
      {
         stream->reader = nullptr;
         stream->delete_reader();
      }
   }

   asio::any_io_executor get_executor() const noexcept override
   {
      assert(stream);
      return stream->get_executor();
   }

   std::optional<size_t> content_length() const noexcept override
   {
      return stream ? stream->content_length : std::nullopt;
   }

   unsigned int status_code() const noexcept override { return stream ? stream->status_code : 0; }

   boost::url_view url() const override
   {
      assert(stream);
      return stream->url;
   }

   void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) override
   {
      if (!stream)
      {
         std::move(handler)(boost::beast::http::error::partial_message, 0);
         return;
      }
      if (asio::buffer_size(buffer) == 0)
      {
         asio::any_completion_executor ex =
            asio::get_associated_immediate_executor(handler, stream->get_executor());
         ex.execute([handler = std::move(handler)]() mutable
         { std::move(handler)(boost::system::error_code{}, 0); });
         return;
      }

      auto cs = asio::get_associated_cancellation_slot(handler);
      if (cs.is_connected() && !cs.has_handler())
      {
         cs.assign([this](asio::cancellation_type_t)
         {
            if (stream && stream->read_handler)
            {
               asio::post(stream->get_executor(), [handler = std::move(stream->read_handler)]() mutable
               { std::move(handler)(errc::make_error_code(errc::operation_canceled), 0); });
            }
         });
      }

      assert(!stream->read_handler);
      stream->read_handler = std::move(handler);
      stream->read_handler_buffer = buffer;
      stream->call_read_handler();
   }

   void detach() override { stream = nullptr; }

   Http3ClientStream* stream;
};

// =================================================================================================
// Http3ClientStream implementation
// =================================================================================================

Http3ClientStream::Http3ClientStream(Http3ClientSession& s, int64_t stream_id)
   : id(stream_id), session(s)
{
   log_prefix = std::format("{}.{}", session.logPrefix(), id);
   logd("[{}] stream created", log_prefix);
}

Http3ClientStream::~Http3ClientStream()
{
   logd("[{}] stream destroyed...", log_prefix);
   if (read_handler)
      swap_and_invoke(read_handler, errc::make_error_code(errc::connection_reset), 0);
   if (!response_delivered && response_handler)
      swap_and_invoke(response_handler, errc::make_error_code(errc::connection_reset),
                      client::Response{nullptr});
   logd("[{}] stream destroyed... done", log_prefix);
}

asio::any_io_executor Http3ClientStream::get_executor() const noexcept
{
   return session.get_executor();
}

// -------------------------------------------------------------------------------------------------

void Http3ClientStream::on_data_chunk(const uint8_t* data, size_t len)
{
   if (len == 0)
      return;
   pending_read.emplace_back(data, data + len);
   if (read_head.size() == 0)
      read_head = asio::buffer(pending_read.front());
   call_read_handler();
}

void Http3ClientStream::on_eof()
{
   eof_received = true;
   call_read_handler();
}

void Http3ClientStream::call_read_handler()
{
   if (!read_handler)
      return;

   if (asio::buffer_size(read_head) > 0)
   {
      auto copied = asio::buffer_copy(read_handler_buffer, read_head);
      read_head += copied;
      if (read_head.size() == 0)
      {
         pending_read.pop_front();
         read_head =
            pending_read.empty() ? asio::const_buffer{} : asio::buffer(pending_read.front());
      }
      swap_and_invoke(read_handler, boost::system::error_code{}, copied);
      return;
   }

   if (eof_received)
      swap_and_invoke(read_handler, boost::system::error_code{}, 0);
}

// -------------------------------------------------------------------------------------------------

nghttp3_ssize Http3ClientStream::data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags)
{
   if (veccnt == 0)
      return 0;

   if (!pending_writes.empty())
   {
      in_flight_writes.emplace_back(std::move(pending_writes.front()));
      pending_writes.pop_front();
      auto& chunk = in_flight_writes.back();
      vec[0].base = chunk.data();
      vec[0].len = chunk.size();
      if (eof_submitted && pending_writes.empty())
      {
         *pflags |= NGHTTP3_DATA_FLAG_EOF;
         eof_sent_to_h3 = true;
      }
      return 1;
   }

   if (eof_submitted)
   {
      *pflags |= NGHTTP3_DATA_FLAG_EOF;
      eof_sent_to_h3 = true;
      return 0;
   }

   return NGHTTP3_ERR_WOULDBLOCK;
}

void Http3ClientStream::on_write_consumed(size_t /*n*/)
{
   // Nothing to do: user handlers already fired in async_write(), in_flight_writes are freed on
   // stream destruction.
}

// -------------------------------------------------------------------------------------------------

void Http3ClientStream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   if (response_delivered)
   {
      auto ec = asio::error::basic_errors::already_started;
      asio::any_completion_executor ex =
         asio::get_associated_immediate_executor(handler, get_executor());
      ex.execute([handler = std::move(handler), ec]() mutable
      { std::move(handler)(ec, client::Response{nullptr}); });
      return;
   }

   auto cs = handler.get_cancellation_slot();
   if (cs.is_connected())
   {
      cs.assign([this](asio::cancellation_type_t ct)
      {
         logd("[{}] async_get_response: cancelled ({})", log_prefix, ct);
         if (response_handler)
         {
            asio::post(get_executor(), [handler = std::move(response_handler)]() mutable
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

void Http3ClientStream::deliver_response()
{
   if (!headers_received || !response_handler)
      return;

   response_delivered = true;
   auto response = client::Response{std::make_unique<Http3ClientReader>(*this)};
   swap_and_invoke(response_handler, boost::system::error_code{}, std::move(response));
}

void Http3ClientStream::fail(boost::system::error_code ec)
{
   closed = true;
   if (read_handler)
      swap_and_invoke(read_handler, ec, 0);
   if (!headers_received && !response_delivered && response_handler)
   {
      response_delivered = true;
      // A stream closing gracefully (ec success, e.g. NGHTTP3_H3_NO_ERROR) still means no
      // response ever arrived if headers were never received -- never report success with a
      // null Response.
      swap_and_invoke(response_handler, ec ? ec : boost::beast::http::error::end_of_stream,
                      client::Response{nullptr});
   }
   maybe_close();
}

// -------------------------------------------------------------------------------------------------

void Http3ClientStream::delete_reader()
{
   pending_read.clear();
   read_head = {};
   maybe_close();
}

void Http3ClientStream::delete_writer()
{
   if (!eof_submitted)
   {
      eof_submitted = true;
      if (auto h3 = session.h3())
         nghttp3_conn_resume_stream(h3, id);
      session.wake_write();
   }
   maybe_close();
}

void Http3ClientStream::maybe_close()
{
   if (reader || writer)
      return;
   if (!closed)
      return;
   session.erase_stream(id);
}

// =================================================================================================
// Http3ClientSession implementation
// =================================================================================================

Http3ClientSession::Http3ClientSession(asio::any_io_executor executor)
   : executor_(executor), socket_(executor), timer_(executor), ready_signal_(executor)
{
   ngtcp2_ccerr_default(&last_error_);
   // Sentinel timers: expires_at(max) means "not yet"; a wait completes once moved to "min".
   ready_signal_.expires_at(asio::steady_timer::time_point::max());
   logi("Http3ClientSession: ctor");
}

Http3ClientSession::~Http3ClientSession()
{
   timer_.cancel();
   ready_signal_.cancel();
   streams_.clear();
   if (h3_)
      nghttp3_conn_del(h3_);
   if (conn_)
      ngtcp2_conn_del(conn_);
   if (ossl_ctx_)
   {
      if (auto ssl = ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_))
      {
         SSL_set_app_data(ssl, nullptr);
         SSL_free(ssl);
      }
      ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
   }
   logi("Http3ClientSession: dtor");
}

// -------------------------------------------------------------------------------------------------

int Http3ClientSession::init(asio::ip::udp::endpoint remote)
{
   boost::system::error_code ec;
   socket_.open(remote.protocol(), ec);
   if (ec)
   {
      loge("Http3ClientSession::init: open: {}", ec.message());
      return -1;
   }
   socket_.connect(remote, ec);
   if (ec)
   {
      loge("Http3ClientSession::init: connect: {}", ec.message());
      return -1;
   }
   socket_.non_blocking(true, ec);

   auto local = socket_.local_endpoint(ec);
   if (ec)
   {
      loge("Http3ClientSession::init: local_endpoint: {}", ec.message());
      return -1;
   }

   log_prefix_ = std::format("h3c:{}", ngtcp2::util::straddr(remote.data(), remote.size()));

   ngtcp2_cid scid{};
   scid.datalen = 17;
   if (RAND_bytes(scid.data, static_cast<int>(scid.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for SCID failed", log_prefix_);
      return -1;
   }
   ngtcp2_cid dcid{};
   dcid.datalen = 18;
   if (RAND_bytes(dcid.data, static_cast<int>(dcid.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for DCID failed", log_prefix_);
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
   callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
   callbacks.handshake_completed = &Http3ClientSession::cb_handshake_completed;
   callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
   callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
   callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
   callbacks.recv_stream_data = &Http3ClientSession::cb_recv_stream_data;
   callbacks.acked_stream_data_offset = &Http3ClientSession::cb_acked_stream_data_offset;
   callbacks.stream_close = &Http3ClientSession::cb_stream_close;
   callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
   callbacks.extend_max_local_streams_bidi = &Http3ClientSession::cb_extend_max_local_streams_bidi;
   callbacks.rand = &Http3ClientSession::cb_rand;
   callbacks.get_new_connection_id = &Http3ClientSession::cb_get_new_connection_id;
   callbacks.remove_connection_id = &Http3ClientSession::cb_remove_connection_id;
   callbacks.update_key = ngtcp2_crypto_update_key_cb;
   callbacks.stream_stop_sending = &Http3ClientSession::cb_stream_stop_sending;
   callbacks.stream_reset = &Http3ClientSession::cb_stream_reset;
   callbacks.extend_max_stream_data = &Http3ClientSession::cb_extend_max_stream_data;
   callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
   callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
   callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
   callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
   callbacks.recv_rx_key = &Http3ClientSession::cb_recv_rx_key;

   ngtcp2_settings settings;
   ngtcp2_settings_default(&settings);
   settings.initial_ts = ngtcp2::util::timestamp();
   settings.log_printf = &ngtcp2_log_printf;

   ngtcp2_transport_params params;
   ngtcp2_transport_params_default(&params);
   params.initial_max_stream_data_bidi_local = 256_k;
   params.initial_max_stream_data_bidi_remote = 256_k;
   params.initial_max_stream_data_uni = 256_k;
   params.initial_max_data = 1_m;
   params.initial_max_streams_bidi = 100;
   params.initial_max_streams_uni = 3;
   params.max_idle_timeout = std::chrono::nanoseconds(30s).count();

   ngtcp2_path path{
      {local.data(), static_cast<socklen_t>(local.size())},
      {remote.data(), static_cast<socklen_t>(remote.size())},
      nullptr,
   };

   if (auto rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1,
                                        &callbacks, &settings, &params, nullptr, this);
       rv != 0)
   {
      loge("[{}] ngtcp2_conn_client_new: {}", log_prefix_, ngtcp2_strerror(rv));
      return -1;
   }

   auto* ssl = SSL_new(tls_context().ctx);
   if (!ssl)
   {
      loge("[{}] SSL_new failed", log_prefix_);
      return -1;
   }

   conn_ref_.get_conn = &Http3ClientSession::get_conn;
   conn_ref_.user_data = this;
   SSL_set_app_data(ssl, &conn_ref_);
   SSL_set_connect_state(ssl);

   if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_configure_client_session failed", log_prefix_);
      SSL_free(ssl);
      return -1;
   }

   if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_ctx_new failed", log_prefix_);
      SSL_free(ssl);
      return -1;
   }

   ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);

   logi("[{}] connecting, scid={}", log_prefix_, ngtcp2::util::format_hex(scid.data, scid.datalen));
   return 0;
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Http3ClientSession::do_session(Buffer&&)
{
   if (write_streams() != 0)
   {
      signal_ready();
      co_return;
   }
   update_timer();

   std::array<uint8_t, 64_k> buf;
   for (;;)
   {
      boost::system::error_code ec;
      size_t n =
         co_await socket_.async_receive(asio::buffer(buf), redirect_error(use_awaitable, ec));
      if (ec)
      {
         if (ec != asio::error::operation_aborted)
            logw("[{}] receive: {}", log_prefix_, ec.message());
         break;
      }

      if (on_read({buf.data(), n}) != 0)
         break; // handle_error() already tore things down.
   }

   signal_ready(); // no-op if already signalled; unblocks a waiter if handshake never finished.
   co_return;
}

void Http3ClientSession::destroy() noexcept { close(); }

void Http3ClientSession::close()
{
   if (std::exchange(closed_, true))
      return;

   //
   // The connection is going away (user-initiated destroy(), or a protocol/transport error via
   // handle_error()) -- fail every request that hasn't completed yet instead of leaving its
   // async_get_response()/async_read_some() hanging forever. Streams may erase themselves from
   // streams_ as a side effect of fail() (via maybe_close()), so snapshot first.
   //
   std::vector<std::shared_ptr<Http3ClientStream>> streams;
   streams.reserve(streams_.size());
   for (auto& [id, stream] : streams_)
      streams.push_back(stream);
   for (auto& stream : streams)
      stream->fail(errc::make_error_code(errc::connection_reset));

   if (conn_ && !ngtcp2_conn_in_closing_period(conn_) && !ngtcp2_conn_in_draining_period(conn_))
   {
      std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> closebuf;
      ngtcp2_path_storage ps;
      ngtcp2_pkt_info pi;
      ngtcp2_path_storage_zero(&ps);

      auto nwrite = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, closebuf.data(),
                                                        closebuf.size(), &last_error_,
                                                        ngtcp2::util::timestamp());
      if (nwrite > 0)
         send_udp({closebuf.data(), static_cast<size_t>(nwrite)});
   }

   boost::system::error_code ec;
   socket_.cancel(ec);
   timer_.cancel();
   signal_ready();
}

void Http3ClientSession::signal_ready()
{
   ready_signal_.expires_at(asio::steady_timer::time_point::min());
}

// -------------------------------------------------------------------------------------------------

Http3ClientStream* Http3ClientSession::find_stream(int64_t id)
{
   auto it = streams_.find(id);
   return it == streams_.end() ? nullptr : it->second.get();
}

Http3ClientStream* Http3ClientSession::create_stream(int64_t id)
{
   auto [it, inserted] = streams_.emplace(id, std::make_shared<Http3ClientStream>(*this, id));
   return it->second.get();
}

void Http3ClientSession::erase_stream(int64_t id) { streams_.erase(id); }

void Http3ClientSession::wake_write()
{
   // Capture a weak_ptr, not shared_from_this(): wake_write() can be reached from a
   // Reader/Writer destructor that runs as part of *this* session's own teardown, at which
   // point shared_from_this() would throw bad_weak_ptr. See the matching comment in the
   // server's Http3Session::wake_write() (server_impl_udp.cpp).
   asio::post(get_executor(), [self = weak_from_this()]
   {
      auto session = std::static_pointer_cast<Http3ClientSession>(self.lock());
      if (!session || session->closed_)
         return;
      if (session->write_streams() == 0)
         session->update_timer();
   });
}

// -------------------------------------------------------------------------------------------------

void Http3ClientSession::send_udp(std::span<const uint8_t> data)
{
   boost::system::error_code ec;
   socket_.send(asio::buffer(data.data(), data.size()), 0, ec);
   if (ec && ec != asio::error::would_block && ec != asio::error::try_again)
      logw("[{}] send: {}", log_prefix_, ec.message());
}

int Http3ClientSession::on_read(std::span<const uint8_t> data)
{
   logd("[{}] on_read: {} bytes", log_prefix_, data.size());

   ngtcp2_pkt_info pi{};
   auto* path = ngtcp2_conn_get_path(conn_);
   auto rv =
      ngtcp2_conn_read_pkt(conn_, path, &pi, data.data(), data.size(), ngtcp2::util::timestamp());
   if (rv != 0)
   {
      if (rv == NGTCP2_ERR_DRAINING)
         logd("[{}] ngtcp2_conn_read_pkt: draining", log_prefix_);
      else
      {
         logw("[{}] ngtcp2_conn_read_pkt: {}", log_prefix_, ngtcp2_strerror(rv));
         if (rv == NGTCP2_ERR_CRYPTO && !last_error_.error_code)
            ngtcp2_ccerr_set_tls_alert(&last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
         else if (!last_error_.error_code)
            ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      }
      return handle_error(rv);
   }

   if (auto wrv = write_streams(); wrv != 0)
      return wrv;

   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

int Http3ClientSession::write_streams()
{
   if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_))
      return 0;

   std::array<uint8_t, 1500> buf;
   ngtcp2_path_storage ps;
   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   std::array<nghttp3_vec, 16> vec;

   for (;;)
   {
      int64_t stream_id = -1;
      int fin = 0;
      nghttp3_ssize sveccnt = 0;

      if (h3_ && ngtcp2_conn_get_max_data_left(conn_))
      {
         sveccnt = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec.data(), vec.size());
         if (sveccnt < 0)
         {
            loge("[{}] nghttp3_conn_writev_stream: {}", log_prefix_,
                 nghttp3_strerror(static_cast<int>(sveccnt)));
            ngtcp2_ccerr_set_application_error(
               &last_error_, nghttp3_err_infer_quic_app_error_code(static_cast<int>(sveccnt)),
               nullptr, 0);
            return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
         }
      }

      ngtcp2_ssize ndatalen;
      uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
      if (fin)
         flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

      auto nwrite =
         ngtcp2_conn_writev_stream(conn_, &ps.path, &pi, buf.data(), buf.size(), &ndatalen, flags,
                                   stream_id, reinterpret_cast<const ngtcp2_vec*>(vec.data()),
                                   static_cast<size_t>(sveccnt), ngtcp2::util::timestamp());

      if (nwrite < 0)
      {
         switch (nwrite)
         {
         case NGTCP2_ERR_STREAM_DATA_BLOCKED:
            if (h3_ && stream_id >= 0)
               nghttp3_conn_block_stream(h3_, stream_id);
            continue;
         case NGTCP2_ERR_STREAM_SHUT_WR:
            if (h3_ && stream_id >= 0)
               nghttp3_conn_shutdown_stream_write(h3_, stream_id);
            continue;
         case NGTCP2_ERR_WRITE_MORE:
            if (h3_ && stream_id >= 0 && ndatalen > 0)
            {
               if (auto rv =
                      nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
                   rv != 0)
               {
                  loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
                  return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
               }
               if (auto s = find_stream(stream_id))
                  s->on_write_consumed(static_cast<size_t>(ndatalen));
            }
            continue;
         default:
            loge("[{}] ngtcp2_conn_writev_stream: {}", log_prefix_,
                 ngtcp2_strerror(static_cast<int>(nwrite)));
            ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
            return handle_error(static_cast<int>(nwrite));
         }
      }

      if (ndatalen > 0 && h3_ && stream_id >= 0)
      {
         if (auto rv = nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
             rv != 0)
         {
            loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
            return handle_error(NGTCP2_ERR_CALLBACK_FAILURE);
         }
         if (auto s = find_stream(stream_id))
            s->on_write_consumed(static_cast<size_t>(ndatalen));
      }

      if (nwrite == 0)
      {
         ngtcp2_conn_update_pkt_tx_time(conn_, ngtcp2::util::timestamp());
         return 0;
      }

      send_udp({buf.data(), static_cast<size_t>(nwrite)});
   }
}

// -------------------------------------------------------------------------------------------------

void Http3ClientSession::update_timer() { arm_timer_from_ngtcp2(); }

void Http3ClientSession::arm_timer_from_ngtcp2()
{
   if (closed_)
      return;

   auto expiry = ngtcp2_conn_get_expiry(conn_);
   if (expiry == UINT64_MAX)
   {
      timer_.cancel();
      return;
   }

   auto now = ngtcp2::util::timestamp();
   asio::steady_timer::duration delay =
      expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};

   timer_.expires_after(delay);
   timer_.async_wait([self = weak_from_this()](const boost::system::error_code& ec)
   {
      if (ec)
         return;
      if (auto session = std::static_pointer_cast<Http3ClientSession>(self.lock()))
         session->handle_expiry();
   });
}

int Http3ClientSession::handle_expiry()
{
   auto now = ngtcp2::util::timestamp();
   if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
   {
      logw("[{}] ngtcp2_conn_handle_expiry: {}", log_prefix_, ngtcp2_strerror(rv));
      ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }
   if (auto rv = write_streams(); rv != 0)
      return rv;
   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

int Http3ClientSession::handle_error(int /*rv*/)
{
   close();
   return -1;
}

// -------------------------------------------------------------------------------------------------
// ngtcp2 callback implementations
// -------------------------------------------------------------------------------------------------

int Http3ClientSession::cb_handshake_completed(ngtcp2_conn*, void* user)
{
   auto self = static_cast<Http3ClientSession*>(user);
   logi("[{}] TLS handshake complete", self->log_prefix_);
   if (!self->h3_ && self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int Http3ClientSession::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                            uint64_t offset, const uint8_t* data, size_t datalen,
                                            void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   logd("[{}] cb_recv_stream_data: stream={} offset={} datalen={} fin={}", self->log_prefix_,
        stream_id, offset, datalen, !!(flags & NGTCP2_STREAM_DATA_FLAG_FIN));
   if (!self->h3_)
      return 0;

   auto nread = nghttp3_conn_read_stream(self->h3_, stream_id, data, datalen,
                                         (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0);
   if (nread < 0)
   {
      loge("[{}] nghttp3_conn_read_stream({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(static_cast<int>(nread)));
      ngtcp2_ccerr_set_application_error(
         &self->last_error_, nghttp3_err_infer_quic_app_error_code(static_cast<int>(nread)),
         nullptr, 0);
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }

   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, static_cast<uint64_t>(nread));
   ngtcp2_conn_extend_max_offset(self->conn_, static_cast<uint64_t>(nread));
   return 0;
}

int Http3ClientSession::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id,
                                                    uint64_t /*offset*/, uint64_t datalen,
                                                    void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_add_ack_offset(self->h3_, stream_id, datalen); rv != 0)
   {
      loge("[{}] nghttp3_conn_add_ack_offset: {}", self->log_prefix_, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3ClientSession::cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                        uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
      app_error_code = NGHTTP3_H3_NO_ERROR;
   if (self->h3_)
   {
      if (auto rv = nghttp3_conn_close_stream(self->h3_, stream_id, app_error_code); rv != 0)
      {
         if (rv == NGHTTP3_ERR_STREAM_NOT_FOUND)
            return 0;
         loge("[{}] nghttp3_conn_close_stream({}): {}", self->log_prefix_, stream_id,
              nghttp3_strerror(rv));
         return NGTCP2_ERR_CALLBACK_FAILURE;
      }
   }
   return 0;
}

void Http3ClientSession::cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
      std::memset(dest, 0, destlen);
}

int Http3ClientSession::cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token,
                                                 size_t cidlen, void* /*user*/)
{
   if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   cid->datalen = cidlen;
   if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int Http3ClientSession::cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid*, void* /*user*/)
{
   return 0;
}

int Http3ClientSession::cb_extend_max_local_streams_bidi(ngtcp2_conn*, uint64_t /*max_streams*/,
                                                         void* /*user*/)
{
   return 0;
}

int Http3ClientSession::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t /*ec*/,
                                               void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_shutdown_stream_read(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_shutdown_stream_read({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3ClientSession::cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t /*final_size*/,
                                        uint64_t /*ec*/, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_shutdown_stream_read(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_shutdown_stream_read({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3ClientSession::cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id,
                                                  uint64_t /*max_data*/, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_unblock_stream(self->h3_, stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_unblock_stream({}): {}", self->log_prefix_, stream_id,
           nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3ClientSession::cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user)
{
   if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
      return 0;
   auto self = static_cast<Http3ClientSession*>(user);
   if (!self->h3_ && self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

// -------------------------------------------------------------------------------------------------

int Http3ClientSession::setup_http3()
{
   if (h3_)
      return 0;

   nghttp3_callbacks h3cb{};
   h3cb.stream_close = &Http3ClientSession::h3_cb_stream_close;
   h3cb.recv_data = &Http3ClientSession::h3_cb_recv_data;
   h3cb.deferred_consume = &Http3ClientSession::h3_cb_deferred_consume;
   h3cb.begin_headers = &Http3ClientSession::h3_cb_begin_headers;
   h3cb.recv_header = &Http3ClientSession::h3_cb_recv_header;
   h3cb.end_headers = &Http3ClientSession::h3_cb_end_headers;
   h3cb.end_stream = &Http3ClientSession::h3_cb_end_stream;
   h3cb.stop_sending = &Http3ClientSession::h3_cb_stop_sending;
   h3cb.reset_stream = &Http3ClientSession::h3_cb_reset_stream;

   nghttp3_settings settings;
   nghttp3_settings_default(&settings);
   settings.qpack_max_dtable_capacity = 4096;
   settings.qpack_blocked_streams = 100;

   if (auto rv = nghttp3_conn_client_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
   {
      loge("[{}] nghttp3_conn_client_new: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   int64_t ctrl_stream_id = -1;
   if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr); rv != 0)
   {
      loge("[{}] open control stream: {}", log_prefix_, ngtcp2_strerror(rv));
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_control_stream(h3_, ctrl_stream_id); rv != 0)
   {
      loge("[{}] nghttp3_conn_bind_control_stream: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   int64_t qpack_enc_stream_id = -1;
   int64_t qpack_dec_stream_id = -1;
   if (ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr) != 0 ||
       ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr) != 0)
   {
      loge("[{}] open qpack streams failed", log_prefix_);
      return -1;
   }
   if (auto rv = nghttp3_conn_bind_qpack_streams(h3_, qpack_enc_stream_id, qpack_dec_stream_id);
       rv != 0)
   {
      loge("[{}] nghttp3_conn_bind_qpack_streams: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   logi("[{}] HTTP/3 ready (ctrl={} qpack_enc={} qpack_dec={})", log_prefix_, ctrl_stream_id,
        qpack_enc_stream_id, qpack_dec_stream_id);
   signal_ready();
   return 0;
}

// -------------------------------------------------------------------------------------------------
// nghttp3 callbacks
// -------------------------------------------------------------------------------------------------

int Http3ClientSession::h3_cb_stream_close(nghttp3_conn*, int64_t stream_id,
                                           uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   logd("[{}] h3 stream {} closed", self->log_prefix_, stream_id);
   if (auto s = self->find_stream(stream_id))
   {
      auto ec = (app_error_code == NGHTTP3_H3_NO_ERROR)
                   ? boost::system::error_code{}
                   : errc::make_error_code(errc::connection_reset);
      s->fail(ec);
   }
   return 0;
}

int Http3ClientSession::h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data,
                                        size_t datalen, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   ngtcp2_conn_extend_max_offset(self->conn_, datalen);
   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, datalen);
   if (auto s = self->find_stream(stream_id))
      s->on_data_chunk(data, datalen);
   return 0;
}

int Http3ClientSession::h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                               void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, nconsumed);
   ngtcp2_conn_extend_max_offset(self->conn_, nconsumed);
   return 0;
}

int Http3ClientSession::h3_cb_begin_headers(nghttp3_conn*, int64_t /*stream_id*/, void* /*user*/,
                                            void*)
{
   // Nothing to do: the stream (and its Http3ClientStream) was already created synchronously in
   // async_submit(), before the request headers were even submitted to nghttp3. Compare the
   // server, where begin_headers is what creates the stream for a newly-received request.
   return 0;
}

int Http3ClientSession::h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t /*token*/,
                                          nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                                          uint8_t /*flags*/, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   auto n = nghttp3_rcbuf_get_buf(name);
   auto v = nghttp3_rcbuf_get_buf(value);
   auto name_view = std::string_view{reinterpret_cast<const char*>(n.base), n.len};
   auto value_view = std::string_view{reinterpret_cast<const char*>(v.base), v.len};

   auto s = self->find_stream(stream_id);
   if (!s)
      return 0;

   logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", s->log_prefix, name_view, value_view);

   try
   {
      if (name_view == ":status")
      {
         unsigned int status = 0;
         if (std::from_chars(value_view.begin(), value_view.end(), status).ec == std::errc{})
            s->status_code = status;
      }
      else if (name_view == "content-length")
      {
         size_t len = 0;
         if (std::from_chars(value_view.begin(), value_view.end(), len).ec == std::errc{})
            s->content_length = len;
      }
      else
         s->response_fields.set(name_view, value_view);
   }
   catch (const std::exception& ex)
   {
      logw("[{}] ignoring invalid header: {} ({})", s->log_prefix, value_view, ex.what());
   }
   return 0;
}

int Http3ClientSession::h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/,
                                          void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   auto s = self->find_stream(stream_id);
   if (!s)
      return 0;

   logd("[{}] response headers: status={}", s->log_prefix, s->status_code);
   s->headers_received = true;
   s->deliver_response();
   return 0;
}

int Http3ClientSession::h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_eof();
   return 0;
}

int Http3ClientSession::h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id,
                                           uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   ngtcp2_conn_shutdown_stream_read(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

int Http3ClientSession::h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id,
                                           uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3ClientSession*>(user);
   ngtcp2_conn_shutdown_stream_write(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

// -------------------------------------------------------------------------------------------------

namespace
{
nghttp3_ssize client_stream_read_data(nghttp3_conn*, int64_t /*stream_id*/, nghttp3_vec* vec,
                                      size_t veccnt, uint32_t* pflags, void* /*conn_user*/,
                                      void* stream_user)
{
   auto s = static_cast<Http3ClientStream*>(stream_user);
   return s->data_reader(vec, veccnt, pflags);
}
} // namespace

void Http3ClientSession::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                      const Fields& headers)
{
   if (closed_ || !h3_)
   {
      loge("[{}] async_submit: session not ready", log_prefix_);
      std::move(handler)(errc::make_error_code(errc::operation_canceled), client::Request{nullptr});
      return;
   }

   int64_t stream_id = -1;
   if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr); rv != 0)
   {
      loge("[{}] async_submit: ngtcp2_conn_open_bidi_stream: {}", log_prefix_,
           ngtcp2_strerror(rv));
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   auto* stream = create_stream(stream_id);
   stream->url = url;

   //
   // TODO: CONNECT / other methods -- mirrors the h2 client's NGHttp2Session::async_submit(),
   // which is likewise hard-coded to POST.
   //
   std::string method("POST");
   std::string scheme(url.scheme());
   std::string target(url.encoded_target());
   std::string authority(url.host_address());

   std::vector<nghttp3_nv> nva;
   nva.reserve(16); // small typical header count; vector will grow if needed
   nva.push_back(make_nv(":method", method));
   nva.push_back(make_nv(":scheme", scheme));
   nva.push_back(make_nv(":path", target));
   nva.push_back(make_nv(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}': setting pseudo headers is not allowed",
              stream->log_prefix, item.name_string());
      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   nghttp3_data_reader dr{};
   dr.read_data = &client_stream_read_data;

   if (auto rv = nghttp3_conn_submit_request(h3_, stream_id, nva.data(), nva.size(), &dr, stream);
       rv != 0)
   {
      loge("[{}] nghttp3_conn_submit_request: {}", log_prefix_, nghttp3_strerror(rv));
      erase_stream(stream_id);
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   logd("[{}] submit: new stream ID: {}", stream->log_prefix, stream_id);
   wake_write();

   post(get_executor(), [handler = std::move(handler),
                         writer = std::make_unique<Http3ClientWriter>(*stream)]() mutable
   { std::move(handler)(boost::system::error_code{}, client::Request{std::move(writer)}); });
}

// =================================================================================================
// Entry point used by Client::Impl::async_connect() for Protocol::h3.
// =================================================================================================

awaitable<std::shared_ptr<Session::Impl>> async_connect_http3(asio::any_io_executor executor,
                                                               std::string host, std::string port)
{
   boost::asio::ip::udp::resolver resolver(executor);
   auto flags = boost::asio::ip::udp::resolver::numeric_service;
   auto results = co_await resolver.async_resolve(host, port, flags); // may throw

   auto session = std::make_shared<Http3ClientSession>(executor);
   if (session->init(results.begin()->endpoint()) != 0)
      throw boost::system::system_error(errc::make_error_code(errc::connection_refused));

   std::shared_ptr<Session::Impl> impl = session;

#if 1
   co_spawn(executor, impl->do_session(Buffer{}), [impl](const std::exception_ptr& ex) mutable
   {
      if (ex)
         logw("client run: {}", what(ex));
      else
         logi("client run: done");
      impl.reset();
   });
#endif

   //
   // Note: wait_ready() uses a sentinel steady_timer as a one-shot gate (see the comment on
   // ready_signal_ / signal_ready()). Rearming a timer that already has a pending async_wait()
   // cancels that wait with operation_aborted rather than completing it successfully -- so the
   // *error code* here doesn't tell us anything; whether the handshake actually succeeded is
   // reflected in ready() instead.
   //
   boost::system::error_code ec;
   co_await session->wait_ready(redirect_error(use_awaitable, ec));
   if (!session->ready())
      throw boost::system::system_error(errc::make_error_code(errc::connection_refused));

   co_return std::static_pointer_cast<Session::Impl>(session);
}

// =================================================================================================

} // namespace anyhttp::client
