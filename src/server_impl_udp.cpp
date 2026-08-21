//
// anyhttp QUIC / HTTP/3 server.
//
// One `Http3Session` per QUIC connection implements `Session::Impl`, and per-request
// `Http3Stream` state feeds an `Http3Reader` (server::Request) and `Http3Writer`
// (server::Response) into the same `RequestHandler` used by the HTTP/1.1 and HTTP/2
// backends.
//
// Not yet implemented: retry tokens, version negotiation, stateless reset, connection
// migration, ECN, client-side (async_submit is a no-op).
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/literals.hpp"
#include "anyhttp/request_handlers.hpp" // IWYU pragma: keep
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/url/parse.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <net/if.h>
#include <netinet/udp.h>
#include <sys/socket.h>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <algorithm>
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

namespace anyhttp::server
{

// =================================================================================================

struct Endpoint
{
   ngtcp2::Address addr;
   int fd;
};

// =================================================================================================
// Free-standing helpers
// =================================================================================================

namespace
{

constexpr size_t QUIC_SCIDLEN = 18;

//
// One-shot process-wide initialization of ngtcp2_crypto_ossl and the OpenSSL SSL_CTX
// used for every QUIC connection.
//
struct TlsServerContext
{
   TlsServerContext()
   {
      static const int init_once = []
      {
         if (ngtcp2_crypto_ossl_init() != 0)
            throw std::runtime_error("ngtcp2_crypto_ossl_init");
         return 0;
      }();
      (void)init_once;

      ctx = SSL_CTX_new(TLS_server_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new");

      SSL_CTX_set_options(ctx, (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                                  SSL_OP_SINGLE_ECDH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE |
                                  SSL_OP_NO_ANTI_REPLAY);
      SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);

      //
      // Prefer AES-128 over AES-256 GCM, like ngtcp2's example server does. Combined with
      // SSL_OP_CIPHER_SERVER_PREFERENCE above, this is what actually picks the bulk cipher.
      //
      if (SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                                        "TLS_CHACHA20_POLY1305_SHA256") != 1)
         throw std::runtime_error(std::string{"SSL_CTX_set_ciphersuites: "} +
                                  ERR_error_string(ERR_get_error(), nullptr));

      SSL_CTX_set_alpn_select_cb(ctx, &TlsServerContext::alpn_select_cb, nullptr);

      if (SSL_CTX_use_PrivateKey_file(ctx, "pki/out/server-key.pem", SSL_FILETYPE_PEM) != 1)
         throw std::runtime_error(std::string{"SSL_CTX_use_PrivateKey_file: "} +
                                  ERR_error_string(ERR_get_error(), nullptr));

      if (SSL_CTX_use_certificate_chain_file(ctx, "pki/out/server-chain.pem") != 1)
         throw std::runtime_error(std::string{"SSL_CTX_use_certificate_chain_file: "} +
                                  ERR_error_string(ERR_get_error(), nullptr));

      if (SSL_CTX_check_private_key(ctx) != 1)
         throw std::runtime_error("SSL_CTX_check_private_key");
   }

   ~TlsServerContext()
   {
      if (ctx)
         SSL_CTX_free(ctx);
   }

   TlsServerContext(const TlsServerContext&) = delete;
   TlsServerContext& operator=(const TlsServerContext&) = delete;

   static int alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                             const unsigned char* in, unsigned int inlen, void*)
   {
      for (auto s = std::span{in, inlen}; s.size() >= 3; s = s.subspan(s[0] + 1))
      {
         if (s[0] == 2 && s[1] == 'h' && s[2] == '3')
         {
            *out = &s[1];
            *outlen = 2;
            return SSL_TLSEXT_ERR_OK;
         }
      }
      return SSL_TLSEXT_ERR_ALERT_FATAL;
   }

   SSL_CTX* ctx = nullptr;
};

TlsServerContext& tls_context()
{
   static TlsServerContext instance;
   return instance;
}

// -------------------------------------------------------------------------------------------------

std::string cid_key(const ngtcp2_cid& cid)
{
   return std::string{reinterpret_cast<const char*>(cid.data), cid.datalen};
}

std::string cid_key(const uint8_t* data, size_t len)
{
   return std::string{reinterpret_cast<const char*>(data), len};
}

// -------------------------------------------------------------------------------------------------

int send_udp(int fd, const sockaddr* sa, socklen_t salen, std::span<const uint8_t> data)
{
   for (;;)
   {
      auto n = ::sendto(fd, data.data(), data.size(), 0, sa, salen);
      if (n == -1)
      {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; // best-effort; ngtcp2 will retransmit
         loge("sendto: {}", strerror(errno));
         return -1;
      }
      return 0;
   }
}

// -------------------------------------------------------------------------------------------------

// Sends a run of same-sized packets (as produced by ngtcp2_conn_write_aggregate_pkt2(), all but
// the last exactly `gso_size` bytes) with a single sendmsg() using UDP_SEGMENT (GSO), so N QUIC
// packets cost one syscall instead of N. Falls back to one sendto() per segment -- and remembers
// to do so from then on -- if the kernel/NIC doesn't support UDP_SEGMENT here.
int send_udp_gso(int fd, const sockaddr* sa, socklen_t salen, std::span<const uint8_t> data,
                 size_t gso_size, bool& no_gso)
{
   if (no_gso || data.size() <= gso_size)
   {
      for (; !data.empty();)
      {
         auto len = std::min(gso_size, data.size());
         if (send_udp(fd, sa, salen, data.first(len)) != 0)
            return -1;
         data = data.subspan(len);
      }
      return 0;
   }

   iovec msg_iov{const_cast<uint8_t*>(data.data()), data.size()};
   uint8_t msg_ctrl[CMSG_SPACE(sizeof(uint16_t))];
   msghdr msg{};
   msg.msg_name = const_cast<sockaddr*>(sa);
   msg.msg_namelen = salen;
   msg.msg_iov = &msg_iov;
   msg.msg_iovlen = 1;
   msg.msg_control = msg_ctrl;
   msg.msg_controllen = sizeof(msg_ctrl);

   auto* cm = CMSG_FIRSTHDR(&msg);
   cm->cmsg_level = SOL_UDP;
   cm->cmsg_type = UDP_SEGMENT;
   cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
   auto seg = static_cast<uint16_t>(gso_size);
   memcpy(CMSG_DATA(cm), &seg, sizeof(seg));

   for (;;)
   {
      auto n = ::sendmsg(fd, &msg, 0);
      if (n == -1)
      {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; // best-effort; ngtcp2 will retransmit
         if (errno == EINVAL || errno == EOPNOTSUPP)
         {
            // GSO unsupported on this socket/NIC: fall back permanently and resend as
            // individual datagrams.
            no_gso = true;
            return send_udp_gso(fd, sa, salen, data, gso_size, no_gso);
         }
         loge("sendmsg (GSO): {}", strerror(errno));
         return -1;
      }
      return 0;
   }
}

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

/// Logs a block of headers, one per line, in the same style as the received ones.
void log_headers(std::string_view log_prefix, const std::vector<nghttp3_nv>& nva)
{
   for (const auto& nv : nva)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", log_prefix,
           std::string_view(reinterpret_cast<const char*>(nv.name), nv.namelen),
           std::string_view(reinterpret_cast<const char*>(nv.value), nv.valuelen));
}

/// Same, for a header block buffered up by the recv_header callback.
void log_headers(std::string_view log_prefix,
                 const std::vector<std::pair<std::string, std::string>>& headers)
{
   for (const auto& [name, value] : headers)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", log_prefix, name, value);
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
// Http3Stream: per-request state.
// =================================================================================================

class Http3Session;
class Http3Stream;

class Http3Stream : public std::enable_shared_from_this<Http3Stream>
{
public:
   Http3Stream(Http3Session& session, int64_t id);
   ~Http3Stream();

   int64_t id;
   Http3Session& session;
   std::string log_prefix;

   //
   // Request state (populated by nghttp3 header callbacks).
   //
   std::string method;
   boost::urls::url url;
   std::optional<size_t> content_length;
   Fields request_fields;

   //
   // The header block as it arrived, buffered so that h3_cb_end_headers() can log it in one go,
   // below the request line, instead of one stray line per header as they come in. Only filled
   // when debug logging is on, and dropped again as soon as it has been logged.
   //
   std::vector<std::pair<std::string, std::string>> received_headers;

   //
   // Response state (populated by user via Http3Writer).
   //
   unsigned int response_status = 0;
   Fields response_fields;
   std::optional<size_t> response_content_length;
   std::string response_content_length_str; // storage for nghttp3_nv
   bool response_submitted = false;

   //
   // Request body plumbing (client → server).
   //
   std::deque<std::vector<uint8_t>> pending_read;
   asio::const_buffer read_head; // view of pending_read.front() not yet delivered
   asio::const_buffer incoming; // chunk on_data_chunk() is delivering, not yet taken
   bool eof_received = false;
   ReadSomeHandler read_handler;
   asio::mutable_buffer read_handler_buffer;
   bool call_read_handler_active = false; // re-entrancy guard, see call_read_handler()

   //
   // Response body plumbing (server → client). Only one async_write() may be active at a time --
   // callers must wait for its handler before issuing another (same contract as e.g. Beast) -- so
   // this is flat per-stream state rather than a queue of pending writes. See the client-side
   // counterpart (Http3ClientStream in client_impl_udp.cpp) for the fuller rationale.
   //
   // write_source is the caller's buffer, passed on to nghttp3 by reference: data_reader() points
   // the nghttp3_vec straight into it, so the response body is never copied on its way down to the
   // nghttp3/ngtcp2 boundary, however large it is -- the mmap()ed file of serve_file() travels
   // from the page cache into QUIC packets without an intermediate byte.
   //
   // What that costs is *when* the write completes. ngtcp2 keeps pointing into this memory for as
   // long as the bytes may still have to be retransmitted (it only ever copies the nghttp3_vec
   // descriptors, never the payload), and running the write handler is what releases the caller's
   // buffer -- so the handler has to wait for the data to be acknowledged. This is the model
   // nghttp3 documents for its read_data callback: "the application must retain data until they
   // are safe to free; it is notified by nghttp3_acked_stream_data". HTTP/2 completes a write as
   // soon as nghttp2 has copied it into its own frame buffer, so a single async_write() there is
   // done roughly a memcpy later, and here roughly a round trip later.
   //
   // write_offered tracks how much of write_source has been handed to nghttp3, which may ask
   // again before any of it goes out and would take a repeated offer as *additional*, distinct
   // stream bytes -- duplicating the body on the wire -- so a repeat call gets
   // NGHTTP3_ERR_WOULDBLOCK instead. write_acked tracks what came back through nghttp3's
   // acked_stream_data callback; the write is complete once that has caught up with write_source.
   //
   bool write_active = false;
   asio::const_buffer write_source;
   size_t write_offered = 0;
   size_t write_acked = 0;
   bool write_is_eof = false;
   WriteHandler write_handler;
   uint64_t write_token = 0;
   uint64_t next_write_token = 1;
   bool eof_submitted = false; // user signalled EOF via empty write
   bool eof_sent_to_h3 = false; // NGHTTP3_DATA_FLAG_EOF returned

   //
   // Lifecycle.
   //
   impl::Reader* reader = nullptr; // pointer back to the Http3Reader when attached
   impl::Writer* writer = nullptr; // pointer back to the Http3Writer when attached
   bool closed = false; // set in h3_cb_stream_close

   asio::any_io_executor get_executor() const noexcept;

   const std::string& logPrefix() const noexcept { return log_prefix; }

   // Data flow into user land.
   void on_data_chunk(const uint8_t* data, size_t len);
   void on_eof();
   void call_read_handler();

   // Data flow from user land back to nghttp3.
   void submit_response();
   void start_write(WriteHandler&& handler, asio::const_buffer buffer);
   nghttp3_ssize data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags);
   void on_write_acked(size_t n);

private:
   void bind_write_cancellation(WriteHandler& handler, uint64_t token); // arms cancellation
   void finish_active_write(); // completes the active write and releases the caller's buffer

public:
   // Called from either reader or writer destructor.
   void delete_reader();
   void delete_writer();
   void maybe_close();
};

// =================================================================================================
// Http3Session: one QUIC connection, one anyhttp Session::Impl.
// =================================================================================================

class Http3Session : public Session::Impl
{
public:
   Http3Session(Server::Impl& server, Endpoint ep, ngtcp2::Address remote);
   ~Http3Session() override;

   //
   // Session::Impl
   //
   asio::any_io_executor get_executor() const noexcept override { return server_.get_executor(); }
   void async_submit(SubmitHandler&& handler, boost::urls::url, const Fields&) override;
   awaitable<void> do_session(Buffer&& data) override;
   void destroy() noexcept override;

   //
   // Interface used by the UDP demux in Server::Impl.
   //
   int init(const ngtcp2_cid& dcid, const ngtcp2_cid& scid, uint32_t version,
            const ngtcp2_pkt_info& pi, std::span<const uint8_t> data);
   int on_read(const ngtcp2_pkt_info& pi, std::span<const uint8_t> data,
               const ngtcp2::Address& remote);
   int write_streams();

   //
   // on_read() does not write; it only marks the session here, and Server::Impl calls
   // flush_write() once the whole receive batch has been fed to ngtcp2. Writing per datagram
   // means every aggregate pass only sees what happened to be queued at that instant, so a
   // response goes out as several small GSO batches instead of one big one.
   //
   void defer_write() noexcept { write_pending_ = true; }
   int flush_write();

   ngtcp2_ssize write_pkt(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                          ngtcp2_tstamp ts);
   void update_timer();
   int handle_expiry();

   const ngtcp2_cid& scid() const noexcept { return scid_; }
   ngtcp2_conn* conn() const noexcept { return conn_; }
   bool closed() const noexcept { return closed_; }
   const std::string& logPrefix() const noexcept { return log_prefix_; }
   Server::Impl& server() noexcept { return server_; }
   nghttp3_conn* h3() const noexcept { return h3_; }

   // Called by Http3Writer/Reader to make sure the write loop runs after new data was queued.
   void wake_write();

   //
   // Grants the peer more *stream*-level send credit for `n` bytes of request body just delivered
   // to the application. Deliberately NOT called as data arrives (see h3_cb_recv_data) -- only
   // once call_read_handler() actually hands bytes to the app, so a slow/absent reader keeps the
   // peer's flow control window for *this stream* genuinely constrained instead of nghttp3
   // buffering an unbounded backlog in pending_read. Connection-level credit is granted eagerly
   // regardless (see h3_cb_recv_data) since it's a pool shared with control/QPACK streams nghttp3
   // manages on its own.
   //
   void consume_stream(int64_t stream_id, size_t n)
   {
      if (n == 0)
         return;
      ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, n);
      wake_write(); // a WINDOW_UPDATE-equivalent frame needs to go out
   }

   //
   // Abort both directions of the stream (RESET_STREAM + STOP_SENDING), the QUIC equivalent of
   // HTTP/2's RST_STREAM. nghttp3 learns of the dead write side through the existing
   // NGTCP2_ERR_STREAM_SHUT_WR handling in write_streams().
   //
   void reset_stream(int64_t stream_id, uint64_t app_error_code)
   {
      ngtcp2_conn_shutdown_stream(conn_, 0, stream_id, app_error_code);
      wake_write();
   }

   //
   // Half-close just our read direction (STOP_SENDING), telling the peer to stop sending the
   // request body while the response we are still writing keeps flowing. Fires the local
   // stream_stop_sending callback, which is what tells nghttp3 about it.
   //
   void stop_reading(int64_t stream_id, uint64_t app_error_code)
   {
      ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
      wake_write();
   }

   // Called from udp_on_read() when a packet arrives during the closing period.
   void resend_conn_close();

   //
   // Returns a shared_ptr, not a raw pointer: callers routinely invoke user handlers on the
   // stream they looked up, and those can drop the last reference to it (the coroutine they
   // resume destroying its Request/Response), which erases the stream from streams_. Holding
   // an owning reference for the duration of the lookup keeps that from becoming a
   // use-after-free.
   //
   std::shared_ptr<Http3Stream> find_stream(int64_t id);
   Http3Stream* create_stream(int64_t id);
   void erase_stream(int64_t id);

   //
   // ngtcp2 <-> ngtcp2_crypto_ossl bridge.
   //
   static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref)
   {
      return static_cast<Http3Session*>(ref->user_data)->conn_;
   }

   //
   // ngtcp2 callback bridges
   //
   static int cb_handshake_completed(ngtcp2_conn*, void* user);
   static int cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t offset,
                                  const uint8_t* data, size_t datalen, void* user, void*);
   static int cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset,
                                          uint64_t datalen, void* user, void*);
   static int cb_stream_open(ngtcp2_conn*, int64_t stream_id, void* user);
   static int cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                              uint64_t app_error_code, void* user, void*);
   static void cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*);
   static int cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                       void* user);
   static int cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user);
   static int cb_extend_max_remote_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user);
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
   static int h3_cb_acked_stream_data(nghttp3_conn*, int64_t stream_id, uint64_t datalen,
                                      void* user, void*);
   static int h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data, size_t datalen,
                              void* user, void*);
   static int h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed, void* user,
                                     void*);
   static int h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token,
                                nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t flags,
                                void* user, void*);
   static int h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int fin, void* user, void*);
   static int h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*);
   static int h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);
   static int h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                 void* user, void*);

private:
   int setup_http3();
   int handle_error(int rv);
   void arm_timer_from_ngtcp2();
   void signal_done();
   void schedule_close_timer();

private:
   Server::Impl& server_;
   Endpoint ep_;
   ngtcp2::Address remote_;
   ngtcp2_cid scid_{};

   ngtcp2_conn* conn_ = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr;
   ngtcp2_crypto_conn_ref conn_ref_{};

   nghttp3_conn* h3_ = nullptr;

   asio::steady_timer timer_;
   asio::steady_timer done_signal_; // used to wake do_session() on connection close
   ngtcp2_ccerr last_error_{};
   bool closed_ = false;

   std::string log_prefix_;

   bool write_pending_ = false; // set by on_read(), acted on by flush_write()
   bool write_posted_ = false; // a wake_write() flush is already on the way

   std::vector<uint8_t> conn_closebuf_; // buffered CONNECTION_CLOSE packet

   // Aggregated TX buffer: ngtcp2_conn_write_aggregate_pkt2() packs as many same-sized
   // packets as it can (control/QPACK streams, response data, ...) into this buffer so
   // they can all be flushed with a single sendmsg()+UDP_SEGMENT (GSO) call instead of
   // one sendto() per QUIC packet.
   std::vector<uint8_t> tx_buf_ = std::vector<uint8_t>(64_k);
   bool no_gso_ = false;

   std::unordered_map<int64_t, std::shared_ptr<Http3Stream>> streams_;
};

// =================================================================================================
// Http3Reader / Http3Writer: adapter classes that plug Http3Stream into the anyhttp
// Reader/Writer interfaces. Server-side only; the client-side templates come later.
// =================================================================================================

template <typename Interface>
class Http3Reader : public Interface
{
public:
   explicit Http3Reader(Http3Stream& s) : stream(&s) { s.reader = this; }
   ~Http3Reader() override
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

   unsigned int status_code() const noexcept override
   {
      // Server-side Request; status doesn't apply, but the interface requires it.
      return 0;
   }

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
         std::move(handler)(boost::system::error_code{}, 0);
         return;
      }

      assert(!stream->read_handler);
      stream->read_handler = std::move(handler);
      stream->read_handler_buffer = buffer;
      stream->call_read_handler();
   }

   void detach() override { stream = nullptr; }

   Http3Stream* stream;
};

template <typename Base>
class Http3Writer : public Base
{
public:
   explicit Http3Writer(Http3Stream& s) : stream(&s) { s.writer = this; }
   ~Http3Writer() override
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

   void content_length(std::optional<size_t> len) override
   {
      assert(stream);
      stream->response_content_length = len;
   }

   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(errc::make_error_code(errc::connection_reset));
         return;
      }

      stream->start_write(std::move(handler), buffer);
   }

   void async_submit(StatusHandler&& handler, unsigned int status_code, const Fields& fields)
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(errc::make_error_code(errc::connection_reset));
         return;
      }
      stream->response_status = status_code;
      stream->response_fields = fields;
      stream->submit_response();
      stream->session.wake_write();

      std::move(handler)(boost::system::error_code{});
   }

   void detach() override { stream = nullptr; }

   Http3Stream* stream;
};

// =================================================================================================
// Http3Stream implementation
// =================================================================================================

Http3Stream::Http3Stream(Http3Session& s, int64_t stream_id) : id(stream_id), session(s)
{
   log_prefix = std::format("{}.{}", session.logPrefix(), id);
   logd("\x1b[1;33mStream: ctor\x1b[0m");
}

Http3Stream::~Http3Stream()
{
   mlogd("\x1b[33mStream: dtor... \x1b[0m");
   // A Http3Writer/Http3Reader (owned by the user-visible Request/Response) can outlive this
   // stream, e.g. when the session tears down streams_ while a suspended coroutine still holds
   // one. Detach them so their destructors don't dereference a freed stream.
   if (reader)
      reader->detach();
   if (writer)
      writer->detach();
   if (read_handler)
      swap_and_invoke(read_handler, errc::make_error_code(errc::connection_reset), 0);
   if (write_active && write_handler)
      swap_and_invoke(write_handler, errc::make_error_code(errc::connection_reset));
   mlogd("\x1b[33mStream: dtor... done\x1b[0m");
}

asio::any_io_executor Http3Stream::get_executor() const noexcept { return session.get_executor(); }

// -------------------------------------------------------------------------------------------------

void Http3Stream::on_data_chunk(const uint8_t* data, size_t len)
{
   if (len == 0)
      return;

   //
   // nghttp3 hands us a view into the packet it is parsing, valid only until this callback
   // returns. Offer it to a waiting reader as it stands before copying it anywhere: a handler
   // that keeps a read outstanding -- the usual shape -- takes the bytes with a single copy, and
   // the vector that would otherwise carry them (a malloc, a copy in, a copy out and a free, per
   // QUIC packet, so about fifty of each per 64k of request body) is never created at all. Only
   // what the reader could not take is parked for later.
   //
   auto self = shared_from_this(); // a resumed reader may drop the last reference to this stream
   incoming = asio::const_buffer{data, len};
   call_read_handler();

   if (incoming.size() > 0)
   {
      auto* rest = static_cast<const uint8_t*>(incoming.data());
      pending_read.emplace_back(rest, rest + incoming.size());
      if (read_head.size() == 0)
         read_head = asio::buffer(pending_read.front());
      incoming = {};
   }
}

void Http3Stream::on_eof()
{
   eof_received = true;
   call_read_handler();
}

void Http3Stream::call_read_handler()
{
   //
   // swap_and_invoke() below may resume a user coroutine that calls async_read_some() again
   // before returning, which re-enters this function. Letting that nested call do real work would
   // recurse once per buffered chunk -- with enough data queued up, that blows the C++ stack.
   // Instead, the nested call just re-arms read_handler and returns; the outer call's loop below
   // picks it up and keeps going without growing the stack. See the client-side counterpart,
   // Http3ClientStream::call_read_handler() in client_impl_udp.cpp.
   //
   if (!read_handler || call_read_handler_active)
      return;

   //
   // The loop below may resume a coroutine that drops the last owning reference to this stream --
   // or to the whole Session, when that coroutine was the last user of the connection. Keep both
   // alive until this function returns: consume_stream() at the bottom dereferences the session,
   // so outliving the stream alone is not enough.
   //
   auto self = shared_from_this();
   auto session_guard = session.shared_from_this();

   call_read_handler_active = true;
   size_t consumed = 0;
   while (read_handler)
   {
      if (read_head.size() > 0 || incoming.size() > 0)
      {
         //
         // Fill the caller's buffer from as many queued chunks as it takes, rather than stopping
         // at the end of the first one. Each chunk is what arrived in a single QUIC packet -- a
         // little over a kilobyte -- so handing them out one per read turns a 64k body into ~48
         // reads, and a handler that answers every read with a write (an echo) pays a full
         // round trip for each of them, because a body write only completes once the peer has
         // acknowledged it (see the comment above write_active).
         //
         auto dest = read_handler_buffer;
         size_t copied = 0;
         while (dest.size() > 0 && read_head.size() > 0)
         {
            auto n = asio::buffer_copy(dest, read_head);
            dest += n;
            read_head += n;
            copied += n;
            if (read_head.size() == 0)
            {
               pending_read.pop_front();
               read_head =
                  pending_read.empty() ? asio::const_buffer{} : asio::buffer(pending_read.front());
            }
         }

         //
         // ... and last from the chunk being delivered right now, which on_data_chunk() offers
         // through `incoming` instead of parking it in a vector of its own first. Queued chunks
         // go first: they arrived earlier.
         //
         if (dest.size() > 0 && incoming.size() > 0)
         {
            auto n = asio::buffer_copy(dest, incoming);
            incoming += n;
            copied += n;
         }

         consumed += copied;
         swap_and_invoke(read_handler, boost::system::error_code{}, copied);
         continue;
      }

      if (eof_received)
      {
         // 0-byte read = EOF, matching the beast/nghttp2 convention.
         swap_and_invoke(read_handler, boost::system::error_code{}, 0);
         continue;
      }

      if (closed)
      {
         //
         // The stream died before the request body was complete, and this read was issued after
         // the close -- there is nothing left that could ever complete it, so report the
         // truncation now rather than leaving it pending forever.
         //
         swap_and_invoke(read_handler, boost::beast::http::error::partial_message, 0);
         continue;
      }

      break;
   }
   call_read_handler_active = false;

   //
   // Grant the peer more send credit only for what was actually delivered to the app -- see
   // Http3Session::consume_stream() for why this must not happen any earlier.
   //
   session.consume_stream(id, consumed);
}

// -------------------------------------------------------------------------------------------------

namespace
{
nghttp3_ssize stream_read_data(nghttp3_conn*, int64_t /*stream_id*/, nghttp3_vec* vec,
                               size_t veccnt, uint32_t* pflags, void* /*conn_user*/,
                               void* stream_user)
{
   auto s = static_cast<Http3Stream*>(stream_user);
   return s->data_reader(vec, veccnt, pflags);
}
} // namespace

void Http3Stream::submit_response()
{
   assert(!response_submitted);

   auto status_str = std::to_string(response_status);
   std::vector<nghttp3_nv> nva;
   nva.reserve(16); // small typical header count; vector will grow if needed
   auto date_str = format_http_date(std::chrono::system_clock::now());
   nva.push_back(make_nv(":status", status_str));
   nva.push_back(make_nv("date", date_str));
   nva.push_back(make_nv("server", "anyhttp-quic/0.1"));

   if (response_content_length)
   {
      response_content_length_str = std::to_string(*response_content_length);
      nva.push_back(make_nv("content-length", response_content_length_str));
   }

   for (auto&& item : response_fields)
   {
      if (item.name_string().starts_with(':'))
      {
         logw("[{}] submit_response: dropping pseudo-header '{}'", log_prefix, item.name_string());
         continue;
      }
      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   nghttp3_data_reader dr{};
   dr.read_data = &stream_read_data;

   if (auto rv = nghttp3_conn_set_stream_user_data(session.h3(), id, this); rv != 0)
   {
      loge("[{}] nghttp3_conn_set_stream_user_data: {}", log_prefix, nghttp3_strerror(rv));
      return;
   }

   if (auto rv = nghttp3_conn_submit_response(session.h3(), id, nva.data(), nva.size(), &dr);
       rv != 0)
   {
      loge("[{}] nghttp3_conn_submit_response: {}", log_prefix, nghttp3_strerror(rv));
      return;
   }
   response_submitted = true;

   using namespace boost::beast::http;
   logd("[{}] {} {}", log_prefix, response_status, obsolete_reason(int_to_status(response_status)));
   log_headers(log_prefix, nva);
}

void Http3Stream::start_write(WriteHandler&& handler, asio::const_buffer buffer)
{
   auto n = asio::buffer_size(buffer);
   const bool is_eof = (n == 0);
   logd("[{}] start_write: n={} is_eof={}", log_prefix, n, is_eof);

   //
   // Once accepted, the caller's intent to end the response body is final: this is what tells
   // delete_writer() the body ended where it was meant to, so it need not reset the stream. An
   // earlier cancellation just makes for a shorter body than planned -- legitimate here, and a
   // declared content-length is still enforced by the peer.
   //
   if (is_eof && eof_submitted)
   {
      //
      // The body was already ended. If that FIN is still pending (its handler was detached by
      // cancellation, see bind_write_cancellation()), adopt this handler so it completes when the
      // FIN actually goes out; otherwise the FIN is long gone and there is nothing left to do.
      //
      if (write_active && write_is_eof)
      {
         logd("[{}] start_write: FIN already pending, adopting handler", log_prefix);
         bind_write_cancellation(handler, write_token);
         write_handler = std::move(handler);
      }
      else if (handler)
      {
         asio::any_completion_executor ex =
            asio::get_associated_immediate_executor(handler, get_executor());
         ex.execute([handler = std::move(handler)]() mutable
         { std::move(handler)(boost::system::error_code{}); });
      }
      return;
   }

   //
   // Only one async_write() may be active at a time -- see the class comment above write_active.
   // The re-issued EOF handled above is not an exception to that: it adopts the FIN that is
   // already in flight instead of starting a write of its own, and has returned by now.
   //
   assert(!write_active);

   if (is_eof)
      eof_submitted = true;

   const uint64_t token = next_write_token++;
   bind_write_cancellation(handler, token);

   write_active = true;
   write_source = buffer; // referenced, not copied -- see class comment above write_active
   write_offered = 0;
   write_acked = 0;
   write_is_eof = is_eof;
   write_token = token;
   write_handler = std::move(handler);

   if (auto h3 = session.h3())
      nghttp3_conn_resume_stream(h3, id);
   session.wake_write();
}

void Http3Stream::bind_write_cancellation(WriteHandler& handler, uint64_t token)
{
   // Nothing to bind for a caller that passed no completion handler.
   if (!handler)
      return;

   auto cs = asio::get_associated_cancellation_slot(handler);
   if (!cs.is_connected() || cs.has_handler())
      return;

   cs.assign([this, token](asio::cancellation_type_t ct)
   {
      //
      // Cancellation completes the write immediately, without waiting for the acknowledgements
      // it would normally complete on -- see below for what that costs.
      //
      if (write_token != token || !write_handler)
         return; // already completed naturally before the cancellation was delivered

      if (write_is_eof)
      {
         //
         // The body has already been declared ended, and a FIN cannot be un-sent -- it may just
         // still be waiting for flow control credit. Detach the handler but leave the write
         // active so it still goes out: abandoning it would leave the stream half-open forever,
         // with the peer waiting for an end that never comes.
         //
         logd("[{}] async_write: \x1b[1;31mcancelled\x1b[0m ({}), FIN still pending", log_prefix,
              ct);
         asio::post(get_executor(), [handler = std::move(write_handler)]() mutable { //
            std::move(handler)(errc::make_error_code(errc::operation_canceled));
         });
         return;
      }
      logd("[{}] async_write: \x1b[1;31mcancelled\x1b[0m ({})", log_prefix, ct);

      //
      // The handler runs now, and the caller is free to destroy its buffer the moment it does --
      // but nghttp3/ngtcp2 point straight into that buffer (see the class comment above
      // write_active), so whatever was offered and is not acknowledged yet has to stop being
      // referenced first. Only a reset can guarantee that: RESET_STREAM makes ngtcp2 drop the
      // stream's queued data and keeps it from reclaiming in-flight bytes for retransmission.
      // That costs nothing in expressiveness -- a body cut short mid-write is truncated, which
      // is exactly what delete_writer() resets the stream for as well.
      //
      // A write that never got to offer a byte, or whose bytes are all acknowledged already,
      // leaves nothing behind and lets the stream carry on unharmed.
      //
      if (write_offered > write_acked && !closed)
      {
         logw("[{}] async_write: cancelled with {} bytes unacknowledged, resetting stream",
              log_prefix, write_offered - write_acked);
         session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
         closed = true;
      }
      write_active = false;
      write_source = {};
      // make sure to post this -- otherwise "MAIN COROUTINE DID NOT COMPLETE" happens
      asio::post(get_executor(), [handler = std::move(write_handler)]() mutable
      { std::move(handler)(errc::make_error_code(errc::operation_canceled)); });
   });
}

nghttp3_ssize Http3Stream::data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags)
{
   if (veccnt == 0)
      return 0;

   if (!write_active)
      return NGHTTP3_ERR_WOULDBLOCK;

   //
   // Hand out what is left of the caller's buffer, by reference and in one go: a nghttp3_vec is
   // just a pointer and a length, so there is nothing to be gained from slicing it up, and
   // nghttp3 gets to frame the whole thing as a single DATA frame. It picks up the rest by
   // itself as packets are filled -- see write_pkt(), which keeps feeding the same vec to
   // ngtcp2_conn_writev_stream() and advances nghttp3 by whatever went into the packet.
   //
   const size_t total = asio::buffer_size(write_source);
   if (write_offered < total)
   {
      auto* base = static_cast<const uint8_t*>(write_source.data()) + write_offered;
      vec[0].base = const_cast<uint8_t*>(base); // nghttp3 reads through this, never writes
      vec[0].len = total - write_offered;
      write_offered = total; // don't offer these bytes twice -- see class comment above
                             // write_active
      return 1;
   }

   //
   // Everything has been offered. For a body write there is nothing new until the caller starts
   // the next one (which resumes the stream), so block here rather than returning 0 bytes --
   // returning 0 without NGHTTP3_DATA_FLAG_EOF would tell nghttp3 the body ended.
   //
   if (!write_is_eof)
      return NGHTTP3_ERR_WOULDBLOCK;

   //
   // The EOF marker (write_source is always empty for it) completes as soon as nghttp3 has taken
   // the FIN: unlike body data, a FIN carries no memory of the caller's that we would have to
   // keep alive until it is acknowledged.
   //
   *pflags |= NGHTTP3_DATA_FLAG_EOF;
   eof_sent_to_h3 = true;
   finish_active_write();
   return 0;
}

void Http3Stream::on_write_acked(size_t n)
{
   //
   // n counts *application* data acknowledged on this stream -- nghttp3 accounts for the HTTP/3
   // framing it puts around the body itself, so unlike ngtcp2's stream offsets these bytes are
   // exactly the ones the caller handed us. All of them belong to the write currently active: a
   // write only completes once every byte it offered is acknowledged, so nothing can still be
   // outstanding from an earlier one. Clamp defensively anyway -- an accounting mismatch should
   // complete the write early, not run write_acked past the end of the buffer.
   //
   if (n == 0 || !write_active || write_is_eof)
      return;

   write_acked = std::min(write_acked + n, asio::buffer_size(write_source));
   logd("[{}] on_write_acked: {} bytes, {}/{} acknowledged", log_prefix, n, write_acked,
        asio::buffer_size(write_source));

   if (write_acked == asio::buffer_size(write_source))
      finish_active_write();
}

void Http3Stream::finish_active_write()
{
   assert(write_active);

   //
   // Invoking the handler hands the caller's buffer back to it, so this must only ever run when
   // nothing points into it any more: every offered byte acknowledged (on_write_acked()), or no
   // bytes offered at all (the EOF marker).
   //
   write_source = {};
   auto handler = std::move(write_handler);
   write_active = false;

   if (handler)
      swap_and_invoke(handler, boost::system::error_code{});
}

// -------------------------------------------------------------------------------------------------

void Http3Stream::delete_reader()
{
   auto self = shared_from_this(); // see delete_writer()
   pending_read.clear();
   read_head = {};
   incoming = {};

   //
   // The handler dropped the Request without reading the body to its end (e.g. not_found(), which
   // never looks at it). Stream-level flow control credit is only granted as the application
   // actually reads (see Http3Session::consume_stream()), so a peer with more body to send would
   // stall forever against a window that will now never reopen. Tell it to stop instead:
   // STOP_SENDING half-closes only our read direction, leaving the response we are still writing
   // to flow normally -- HTTP/2 has to submit a full RST_STREAM here for lack of a half-close.
   //
   if (!eof_received && !closed)
   {
      logd("[{}] delete_reader: request body not read to end, sending STOP_SENDING", log_prefix);
      session.stop_reading(id, NGHTTP3_H3_NO_ERROR);
   }

   maybe_close();
}

void Http3Stream::delete_writer()
{
   //
   // The teardown paths below can run handlers that drop the last reference to this stream,
   // erasing it from the session -- keep it alive until this function returns.
   //
   auto self = shared_from_this();

   //
   // Nothing to finalize on a stream ngtcp2 has already torn down (peer reset it, or we did):
   // there is nothing left to reset, and submitting anything would leave nghttp3 holding data for
   // a stream that no longer exists, which it would then offer for sending forever.
   //
   if (closed)
   {
      logd("[{}] delete_writer: stream already closed", log_prefix);
      maybe_close();
      return;
   }

   if (!response_submitted)
   {
      //
      // The handler never even started a response (e.g. it returned, or the request was
      // reset, before calling response.async_submit()). There is no HEADERS frame for
      // nghttp3 to close out, so "synthesize EOF" (below) has nothing to act on and the
      // peer would be left waiting forever. Abort the stream at the transport level
      // instead, mirroring what h3_cb_stop_sending/h3_cb_reset_stream already do for
      // nghttp3-initiated aborts. NO_ERROR here (rather than e.g. INTERNAL_ERROR): the
      // handler choosing not to respond isn't itself a protocol error -- the client just
      // needs to be told the stream is over so it doesn't wait forever.
      //
      ngtcp2_conn_shutdown_stream(session.conn(), 0, id, NGHTTP3_H3_NO_ERROR);
      closed = true;
      session.wake_write();
      maybe_close();
      return;
   }

   if (!eof_submitted)
   {
      //
      // The Response was dropped without ever ending the body (async_write({})), so wherever it
      // stopped is not where it was meant to stop. Sending a FIN here would present that partial
      // response to the client as a complete one -- reset the stream instead, the same way the
      // no-response case above aborts at the transport level, and matching the client's
      // Http3ClientStream::delete_writer().
      //
      logw("[{}] delete_writer: response body never ended, resetting stream", log_prefix);
      session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
      closed = true;
      maybe_close();
      return;
   }
   maybe_close();
}

void Http3Stream::maybe_close()
{
   if (reader || writer)
      return;
   if (!closed)
      return;
   session.erase_stream(id);
}

// =================================================================================================
// Http3Session implementation
// =================================================================================================

Http3Session::Http3Session(Server::Impl& server, Endpoint ep, ngtcp2::Address remote)
   : server_(server), ep_(ep), remote_(remote), timer_(server.get_executor()),
     done_signal_(server.get_executor())
{
   ngtcp2_ccerr_default(&last_error_);
   log_prefix_ = std::format("h3:{}", ngtcp2::util::straddr(&remote_.su.sa, remote_.len));
   // done_signal_ is armed at "never" until signal_done() moves it to the past.
   done_signal_.expires_at(asio::steady_timer::time_point::max());
   mlogi("session created");
}

Http3Session::~Http3Session()
{
   timer_.cancel();
   done_signal_.cancel();
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
   mlogi("session destroyed");
}

// -------------------------------------------------------------------------------------------------

void Http3Session::async_submit(SubmitHandler&& handler, boost::urls::url, const Fields&)
{
   // Client-side submit is not implemented yet.
   std::move(handler)(errc::make_error_code(errc::operation_not_supported),
                      client::Request{nullptr});
}

awaitable<void> Http3Session::do_session(Buffer&&)
{
   boost::system::error_code ec;
   co_await done_signal_.async_wait(redirect_error(use_awaitable, ec));
   // ec is boost::asio::error::operation_aborted (from destroy()) or a spurious
   // wake-up; either way, this coroutine's job is done.
   co_return;
}

void Http3Session::destroy() noexcept
{
   if (std::exchange(closed_, true))
   {
      timer_.cancel();
      signal_done();
      return;
   }

   //
   // Explicit shutdown (e.g. the whole Server::Impl going away): let the peer know right
   // away instead of leaving it to find out via idle timeout (up to 30s). Unlike
   // handle_error(), this doesn't linger for 3 PTO to handle retransmits of the CLOSE --
   // this is a clean, voluntary shutdown, not an error condition worth that effort.
   //
   if (conn_ && !ngtcp2_conn_in_closing_period(conn_) && !ngtcp2_conn_in_draining_period(conn_))
   {
      std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> closebuf;
      ngtcp2_path_storage ps;
      ngtcp2_pkt_info pi;
      ngtcp2_path_storage_zero(&ps);

      auto nwrite =
         ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, closebuf.data(), closebuf.size(),
                                            &last_error_, ngtcp2::util::timestamp());
      if (nwrite > 0)
         send_udp(ep_.fd, ps.path.remote.addr, ps.path.remote.addrlen,
                  {closebuf.data(), static_cast<size_t>(nwrite)});
   }

   timer_.cancel();
   signal_done();
}

void Http3Session::signal_done()
{
   // Move the sentinel timer to the past so any waiter wakes up.
   done_signal_.expires_at(asio::steady_timer::time_point::min());
}

// -------------------------------------------------------------------------------------------------

std::shared_ptr<Http3Stream> Http3Session::find_stream(int64_t id)
{
   auto it = streams_.find(id);
   return it == streams_.end() ? nullptr : it->second;
}

Http3Stream* Http3Session::create_stream(int64_t id)
{
   auto [it, inserted] = streams_.emplace(id, std::make_shared<Http3Stream>(*this, id));
   return it->second.get();
}

void Http3Session::erase_stream(int64_t id) { streams_.erase(id); }

void Http3Session::wake_write()
{
   // The session write loop is only run in reaction to a packet arriving or a timer
   // firing. When the user submits response data outside those events, we need to
   // kick the write loop ourselves.
   //
   // Capture a weak_ptr, not shared_from_this(): wake_write() can be reached from a
   // Reader/Writer destructor that runs as part of *this* session's own teardown (e.g. a
   // still-in-flight request/response destroyed by Server::Impl cancelling everything on
   // shutdown), at which point shared_from_this() would throw bad_weak_ptr.
   //
   // One flush per wake, not one per submission. A response submits its headers, its body and
   // its EOF separately, and posting for each means the first pass writes everything and the
   // rest walk the connection for nothing -- and still re-arm the timer on the way out. Arming
   // once and clearing when the flush runs is what ngtcp2's example server gets for free from
   // ev_io_start() on an already-active watcher.
   //
   if (write_posted_)
      return;
   write_posted_ = true;

   asio::post(get_executor(), [self = weak_from_this()]
   {
      auto session = std::static_pointer_cast<Http3Session>(self.lock());
      if (!session)
         return;
      session->write_posted_ = false;
      if (session->closed_)
         return;
      session->flush_write();
   });
}

// -------------------------------------------------------------------------------------------------

int Http3Session::init(const ngtcp2_cid& dcid, const ngtcp2_cid& scid, uint32_t version,
                       const ngtcp2_pkt_info& pi, std::span<const uint8_t> data)
{
   scid_.datalen = QUIC_SCIDLEN;
   if (RAND_bytes(scid_.data, static_cast<int>(scid_.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for SCID failed", log_prefix_);
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
   callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
   callbacks.handshake_completed = &Http3Session::cb_handshake_completed;
   callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
   callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
   callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
   callbacks.recv_stream_data = &Http3Session::cb_recv_stream_data;
   callbacks.acked_stream_data_offset = &Http3Session::cb_acked_stream_data_offset;
   callbacks.stream_open = &Http3Session::cb_stream_open;
   callbacks.stream_close = &Http3Session::cb_stream_close;
   callbacks.rand = &Http3Session::cb_rand;
   callbacks.get_new_connection_id = &Http3Session::cb_get_new_connection_id;
   callbacks.remove_connection_id = &Http3Session::cb_remove_connection_id;
   callbacks.update_key = ngtcp2_crypto_update_key_cb;
   callbacks.stream_reset = &Http3Session::cb_stream_reset;
   callbacks.extend_max_remote_streams_bidi = &Http3Session::cb_extend_max_remote_streams_bidi;
   callbacks.extend_max_stream_data = &Http3Session::cb_extend_max_stream_data;
   callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
   callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
   callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
   callbacks.stream_stop_sending = &Http3Session::cb_stream_stop_sending;
   callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
   callbacks.recv_rx_key = &Http3Session::cb_recv_rx_key;

   ngtcp2_settings settings;
   ngtcp2_settings_default(&settings);
   settings.initial_ts = ngtcp2::util::timestamp();

   //
   // Only install the log callback when trace logging is actually enabled: ngtcp2 formats every
   // frame of every packet into a string *before* invoking it, so a callback that discards its
   // input still pays for the full formatting. A NULL log_printf makes ngtcp2 skip that work.
   //
   if (spdlog::default_logger_raw()->should_log(spdlog::level::trace))
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
   params.original_dcid = dcid;
   params.original_dcid_present = 1;

   ngtcp2_path path{
      {const_cast<sockaddr*>(&ep_.addr.su.sa), ep_.addr.len},
      {const_cast<sockaddr*>(&remote_.su.sa), remote_.len},
      &ep_,
   };

   if (auto rv = ngtcp2_conn_server_new(&conn_, &scid, &scid_, &path, version, &callbacks,
                                        &settings, &params, nullptr, this);
       rv != 0)
   {
      loge("[{}] ngtcp2_conn_server_new: {}", log_prefix_, ngtcp2_strerror(rv));
      return -1;
   }

   auto* ssl = SSL_new(tls_context().ctx);
   if (!ssl)
   {
      loge("[{}] SSL_new failed", log_prefix_);
      return -1;
   }

   conn_ref_.get_conn = &Http3Session::get_conn;
   conn_ref_.user_data = this;
   SSL_set_app_data(ssl, &conn_ref_);
   SSL_set_accept_state(ssl);

   if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0)
   {
      loge("[{}] ngtcp2_crypto_ossl_configure_server_session failed", log_prefix_);
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

   logi("[{}] new connection, scid={} version=0x{:x}", log_prefix_,
        ngtcp2::util::format_hex(scid_.data, scid_.datalen), version);

   return on_read(pi, data, remote_);
}

// -------------------------------------------------------------------------------------------------

int Http3Session::on_read(const ngtcp2_pkt_info& pi, std::span<const uint8_t> data,
                          const ngtcp2::Address& remote)
{
   logd("[{}] on_read: {} bytes", log_prefix_, data.size());

   ngtcp2_path path{
      {const_cast<sockaddr*>(&ep_.addr.su.sa), ep_.addr.len},
      {const_cast<sockaddr*>(&remote.su.sa), remote.len},
      &ep_,
   };

   auto rv =
      ngtcp2_conn_read_pkt(conn_, &path, &pi, data.data(), data.size(), ngtcp2::util::timestamp());
   if (rv != 0)
   {
      if (rv == NGTCP2_ERR_DRAINING)
         logd("[{}] ngtcp2_conn_read_pkt: draining", log_prefix_);
      else
         logw("[{}] ngtcp2_conn_read_pkt: {}", log_prefix_, ngtcp2_strerror(rv));

      if (rv == NGTCP2_ERR_CRYPTO && !last_error_.error_code)
         ngtcp2_ccerr_set_tls_alert(&last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
      else if (!last_error_.error_code)
         ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }

   //
   // Deliberately no write here -- see defer_write().
   //
   defer_write();
   return 0;
}

// -------------------------------------------------------------------------------------------------

namespace
{
ngtcp2_ssize write_pkt_cb(ngtcp2_conn*, ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                          size_t destlen, ngtcp2_tstamp ts, void* user_data)
{
   return static_cast<Http3Session*>(user_data)->write_pkt(path, pi, dest, destlen, ts);
}
} // namespace

// Writes a single QUIC packet's worth of stream data into [dest, dest+destlen). Called
// repeatedly by ngtcp2_conn_write_aggregate_pkt2() (once per packet it wants to pack into the
// shared TX buffer), so unlike the old single-packet write_streams() this must never call
// send_udp() itself -- the caller decides when/how the accumulated packets go out.
ngtcp2_ssize Http3Session::write_pkt(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                     size_t destlen, ngtcp2_tstamp ts)
{
   std::array<nghttp3_vec, 16> vec;
   int64_t shut_down_stream = -1; // see NGTCP2_ERR_STREAM_NOT_FOUND below

   for (;;)
   {
      int64_t stream_id = -1;
      int fin = 0;
      nghttp3_ssize sveccnt = 0;

      if (h3_ && ngtcp2_conn_get_max_data_left(conn_))
      {
         sveccnt = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec.data(), vec.size());
         logd("[{}] write_pkt: nghttp3_conn_writev_stream -> stream={} sveccnt={} fin={}",
              log_prefix_, stream_id, sveccnt, fin);
         if (sveccnt < 0)
         {
            loge("[{}] nghttp3_conn_writev_stream: {}", log_prefix_,
                 nghttp3_strerror(static_cast<int>(sveccnt)));
            ngtcp2_ccerr_set_application_error(
               &last_error_, nghttp3_err_infer_quic_app_error_code(static_cast<int>(sveccnt)),
               nullptr, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      ngtcp2_ssize ndatalen;
      uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
      if (fin)
         flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

      auto nwrite = ngtcp2_conn_writev_stream(
         conn_, path, pi, dest, destlen, &ndatalen, flags, stream_id,
         reinterpret_cast<const ngtcp2_vec*>(vec.data()), static_cast<size_t>(sveccnt), ts);

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
         case NGTCP2_ERR_STREAM_NOT_FOUND:
            //
            // ngtcp2 has already torn the stream down (the peer reset it, or we did) while
            // nghttp3 still had response data queued for it. That's a dead stream, not a dead
            // connection -- tell nghttp3 so it stops offering it and keep serving the others.
            // Should nghttp3 offer the same stream again anyway, stop packing this packet rather
            // than spinning here forever.
            //
            if (h3_ && stream_id >= 0 && stream_id != shut_down_stream)
            {
               logw("[{}] write_pkt: stream {} is gone, shutting down its write side", log_prefix_,
                    stream_id);
               nghttp3_conn_shutdown_stream_write(h3_, stream_id);
               nghttp3_conn_block_stream(h3_, stream_id);
               shut_down_stream = stream_id;
               continue;
            }
            return 0;
         case NGTCP2_ERR_WRITE_MORE:
            if (h3_ && stream_id >= 0 && ndatalen > 0)
            {
               if (auto rv =
                      nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
                   rv != 0)
               {
                  loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
                  return NGTCP2_ERR_CALLBACK_FAILURE;
               }
            }
            continue;
         default:
            loge("[{}] ngtcp2_conn_writev_stream: {}", log_prefix_,
                 ngtcp2_strerror(static_cast<int>(nwrite)));
            ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      if (ndatalen > 0 && h3_ && stream_id >= 0)
      {
         if (auto rv = nghttp3_conn_add_write_offset(h3_, stream_id, static_cast<size_t>(ndatalen));
             rv != 0)
         {
            loge("[{}] nghttp3_conn_add_write_offset: {}", log_prefix_, nghttp3_strerror(rv));
            return NGTCP2_ERR_CALLBACK_FAILURE;
         }
      }

      return nwrite;
   }
}

// -------------------------------------------------------------------------------------------------

int Http3Session::write_streams()
{
   if (ngtcp2_conn_in_closing_period(conn_) || ngtcp2_conn_in_draining_period(conn_))
      return 0;

   logd("[{}] write_streams: max_data_left={}", log_prefix_, ngtcp2_conn_get_max_data_left(conn_));

   ngtcp2_path_storage ps;
   ngtcp2_pkt_info pi;
   ngtcp2_path_storage_zero(&ps);

   size_t gso_size = 0;
   auto nwrite =
      ngtcp2_conn_write_aggregate_pkt2(conn_, &ps.path, &pi, tx_buf_.data(), tx_buf_.size(),
                                       &gso_size, &write_pkt_cb, 0, ngtcp2::util::timestamp());
   if (nwrite < 0)
   {
      loge("[{}] ngtcp2_conn_write_aggregate_pkt2: {}", log_prefix_,
           ngtcp2_strerror(static_cast<int>(nwrite)));
      if (!last_error_.error_code)
         ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
      return handle_error(static_cast<int>(nwrite));
   }

   ngtcp2_conn_update_pkt_tx_time(conn_, ngtcp2::util::timestamp());

   if (nwrite == 0)
      return 0;

   return send_udp_gso(ep_.fd, ps.path.remote.addr, ps.path.remote.addrlen,
                       {tx_buf_.data(), static_cast<size_t>(nwrite)}, gso_size, no_gso_);
}

// -------------------------------------------------------------------------------------------------

int Http3Session::flush_write()
{
   write_pending_ = false;

   if (closed_ || !conn_)
      return 0;

   if (auto rv = write_streams(); rv != 0)
      return rv;

   update_timer();
   return 0;
}

// -------------------------------------------------------------------------------------------------

void Http3Session::update_timer() { arm_timer_from_ngtcp2(); }

void Http3Session::arm_timer_from_ngtcp2()
{
   if (closed_)
      return;

   auto expiry = ngtcp2_conn_get_expiry(conn_);
   if (expiry == UINT64_MAX)
   {
      // ngtcp2 has no pending timer. Cancel the current one so we don't
      // accidentally keep an old retransmission timer alive past its purpose
      // and don't keep the io_context alive indefinitely.
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
      if (auto session = std::static_pointer_cast<Http3Session>(self.lock()))
         session->handle_expiry();
   });
}

int Http3Session::handle_expiry()
{
   auto now = ngtcp2::util::timestamp();
   if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0)
   {
      logw("[{}] ngtcp2_conn_handle_expiry: {}", log_prefix_, ngtcp2_strerror(rv));
      ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
      return handle_error(rv);
   }
   return flush_write();
}

// -------------------------------------------------------------------------------------------------

int Http3Session::handle_error(int /*rv*/)
{
   if (closed_)
      return -1;
   closed_ = true;

   // Idle timeout and drop-conn need no CONNECTION_CLOSE packet.
   if (last_error_.type == NGTCP2_CCERR_TYPE_IDLE_CLOSE ||
       last_error_.type == NGTCP2_CCERR_TYPE_DROP_CONN)
   {
      signal_done();
      return -1;
   }

   // If already in draining (peer sent CONNECTION_CLOSE), don't reply.
   // If already in closing, the buffered packet is still valid.
   if (conn_ && !ngtcp2_conn_in_draining_period(conn_) && !ngtcp2_conn_in_closing_period(conn_))
   {
      conn_closebuf_.resize(NGTCP2_MAX_UDP_PAYLOAD_SIZE);
      ngtcp2_path_storage ps;
      ngtcp2_pkt_info pi;
      ngtcp2_path_storage_zero(&ps);

      auto nwrite = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, conn_closebuf_.data(),
                                                       conn_closebuf_.size(), &last_error_,
                                                       ngtcp2::util::timestamp());
      if (nwrite > 0)
      {
         conn_closebuf_.resize(static_cast<size_t>(nwrite));
         logi("[{}] sending CONNECTION_CLOSE", log_prefix_);
         send_udp(ep_.fd, ps.path.remote.addr, ps.path.remote.addrlen,
                  {conn_closebuf_.data(), conn_closebuf_.size()});
      }
      else
      {
         conn_closebuf_.clear();
         signal_done();
         return -1;
      }
   }

   // Stay alive for 3 PTO so we can resend the CONNECTION_CLOSE if the
   // peer retransmits, or absorb late packets during the draining period.
   schedule_close_timer();
   return -1;
}

void Http3Session::schedule_close_timer()
{
   auto delay = conn_ ? std::chrono::nanoseconds{ngtcp2_conn_get_pto(conn_) * 3}
                      : std::chrono::milliseconds{100};
   timer_.expires_after(delay);
   timer_.async_wait([self = weak_from_this()](const boost::system::error_code& ec)
   {
      if (ec)
         return;
      auto session = std::static_pointer_cast<Http3Session>(self.lock());
      if (!session)
         return;
      logi("[{}] closing/draining period over", session->log_prefix_);
      session->server_.erase_quic_session(session.get());
      session->signal_done();
   });
}

void Http3Session::resend_conn_close()
{
   if (conn_closebuf_.empty())
      return;
   auto* path = ngtcp2_conn_get_path(conn_);
   if (!path)
      return;
   logd("[{}] resending CONNECTION_CLOSE", log_prefix_);
   send_udp(ep_.fd, path->remote.addr, path->remote.addrlen,
            {conn_closebuf_.data(), conn_closebuf_.size()});
}

// -------------------------------------------------------------------------------------------------
// ngtcp2 callback implementations
// -------------------------------------------------------------------------------------------------

int Http3Session::cb_handshake_completed(ngtcp2_conn*, void* user)
{
   auto self = static_cast<Http3Session*>(user);
   logi("[{}] TLS handshake complete", self->log_prefix_);
   if (self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

int Http3Session::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                      uint64_t offset, const uint8_t* data, size_t datalen,
                                      void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   logd("[{}] cb_recv_stream_data: stream={} offset={} datalen={} fin={} h3_={}", self->log_prefix_,
        stream_id, offset, datalen, !!(flags & NGTCP2_STREAM_DATA_FLAG_FIN), !!self->h3_);
   if (!self->h3_)
   {
      logw("[{}] cb_recv_stream_data: DROPPING {} bytes on stream {} (h3 not ready)",
           self->log_prefix_, datalen, stream_id);
      return 0;
   }

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

int Http3Session::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t /*offset*/,
                                              uint64_t datalen, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_)
      return 0;
   if (auto rv = nghttp3_conn_add_ack_offset(self->h3_, stream_id, datalen); rv != 0)
   {
      loge("[{}] nghttp3_conn_add_ack_offset: {}", self->log_prefix_, nghttp3_strerror(rv));
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int Http3Session::cb_stream_open(ngtcp2_conn*, int64_t /*stream_id*/, void* /*user*/) { return 0; }

int Http3Session::cb_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                  uint64_t app_error_code, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
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

void Http3Session::cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
      std::memset(dest, 0, destlen);
}

int Http3Session::cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, uint8_t* token,
                                           size_t cidlen, void* user)
{
   auto self = static_cast<Http3Session*>(user);
   if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   cid->datalen = cidlen;
   if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   self->server_.associate_quic_cid(*cid, self);
   return 0;
}

int Http3Session::cb_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user)
{
   auto self = static_cast<Http3Session*>(user);
   self->server_.dissociate_quic_cid(*cid);
   return 0;
}

int Http3Session::cb_extend_max_remote_streams_bidi(ngtcp2_conn*, uint64_t /*max_streams*/,
                                                    void* /*user*/)
{
   return 0;
}

int Http3Session::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t /*ec*/,
                                         void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
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

int Http3Session::cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t /*final_size*/,
                                  uint64_t /*ec*/, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
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

int Http3Session::cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t /*max_data*/,
                                            void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
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

int Http3Session::cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user)
{
   if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT)
      return 0;
   auto self = static_cast<Http3Session*>(user);
   if (!self->h3_ && self->setup_http3() != 0)
      return NGTCP2_ERR_CALLBACK_FAILURE;
   return 0;
}

// -------------------------------------------------------------------------------------------------

int Http3Session::setup_http3()
{
   if (h3_)
      return 0;

   nghttp3_callbacks h3cb{};
   h3cb.acked_stream_data = &Http3Session::h3_cb_acked_stream_data;
   h3cb.stream_close = &Http3Session::h3_cb_stream_close;
   h3cb.recv_data = &Http3Session::h3_cb_recv_data;
   h3cb.deferred_consume = &Http3Session::h3_cb_deferred_consume;
   h3cb.begin_headers = &Http3Session::h3_cb_begin_headers;
   h3cb.recv_header = &Http3Session::h3_cb_recv_header;
   h3cb.end_headers = &Http3Session::h3_cb_end_headers;
   h3cb.end_stream = &Http3Session::h3_cb_end_stream;
   h3cb.stop_sending = &Http3Session::h3_cb_stop_sending;
   h3cb.reset_stream = &Http3Session::h3_cb_reset_stream;

   nghttp3_settings settings;
   nghttp3_settings_default(&settings);
   settings.qpack_max_dtable_capacity = 4096;
   settings.qpack_blocked_streams = 100;

   if (auto rv = nghttp3_conn_server_new(&h3_, &h3cb, &settings, nullptr, this); rv != 0)
   {
      loge("[{}] nghttp3_conn_server_new: {}", log_prefix_, nghttp3_strerror(rv));
      return -1;
   }

   auto params = ngtcp2_conn_get_local_transport_params(conn_);
   nghttp3_conn_set_max_client_streams_bidi(h3_, params->initial_max_streams_bidi);

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
   return 0;
}

// -------------------------------------------------------------------------------------------------
// nghttp3 callbacks
// -------------------------------------------------------------------------------------------------

//
// The only notification that the peer is done with response body bytes we handed out by
// reference, and hence that the caller's buffer may be released -- see the comment above
// Http3Stream::write_active.
//
int Http3Session::h3_cb_acked_stream_data(nghttp3_conn*, int64_t stream_id, uint64_t datalen,
                                          void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   auto stream = self->find_stream(stream_id);
   if (!stream)
      return 0;

   //
   // Completing a write resumes the application, which may drop the last reference to this
   // session -- while ngtcp2 is still in the middle of processing the ACK that got us here.
   // weak_from_this(), not shared_from_this(): the ACK may well arrive during teardown.
   //
   auto session_guard = self->weak_from_this().lock();
   stream->on_write_acked(static_cast<size_t>(datalen));
   return 0;
}

int Http3Session::h3_cb_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t /*app_error*/,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   logd("[{}] h3 stream {} closed", self->log_prefix_, stream_id);
   if (auto s = self->find_stream(stream_id))
   {
      s->closed = true;
      // Waiting readers/writers should see the close now.
      if (s->read_handler)
         swap_and_invoke(s->read_handler, boost::system::error_code{}, 0);

      //
      // A write waiting for its data to be acknowledged will never see those acknowledgements
      // now: ngtcp2 drops whatever of this stream is still in flight. It also stops touching the
      // caller's buffer, which is all the wait was ever for, so complete the write -- as failed,
      // because the body did not make it -- instead of leaving it pending forever.
      //
      if (s->write_active && s->write_handler)
      {
         s->write_active = false;
         s->write_source = {};
         swap_and_invoke(s->write_handler, errc::make_error_code(errc::connection_reset));
      }
      s->maybe_close();
   }
   if (ngtcp2_conn_is_server(self->conn_))
      ngtcp2_conn_extend_max_streams_bidi(self->conn_, 1);
   return 0;
}

int Http3Session::h3_cb_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data,
                                  size_t datalen, void* user, void*)
{
   //
   // Connection-level credit is granted immediately: it is a single pool shared with control/QPACK
   // streams that nghttp3 manages on its own (the app never "reads" those), so withholding it here
   // would stall unrelated traffic whenever this one stream's reader is slow. Only the *stream*-
   // level credit for these bytes is deliberately deferred -- see Http3Session::consume_stream().
   // Granting it only once the application actually reads the data (in
   // Http3Stream::call_read_handler()) is what makes request-body backpressure real instead of
   // nghttp3 buffering an unbounded backlog in pending_read while the peer keeps sending on *this*
   // stream.
   //
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_extend_max_offset(self->conn_, datalen);
   if (auto s = self->find_stream(stream_id))
      s->on_data_chunk(data, datalen);
   return 0;
}

int Http3Session::h3_cb_deferred_consume(nghttp3_conn*, int64_t stream_id, size_t nconsumed,
                                         void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_extend_max_stream_offset(self->conn_, stream_id, nconsumed);
   ngtcp2_conn_extend_max_offset(self->conn_, nconsumed);
   return 0;
}

int Http3Session::h3_cb_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   self->create_stream(stream_id);
   return 0;
}

int Http3Session::h3_cb_recv_header(nghttp3_conn*, int64_t stream_id, int32_t /*token*/,
                                    nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t /*flags*/,
                                    void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   auto n = nghttp3_rcbuf_get_buf(name);
   auto v = nghttp3_rcbuf_get_buf(value);
   auto name_view = std::string_view{reinterpret_cast<const char*>(n.base), n.len};
   auto value_view = std::string_view{reinterpret_cast<const char*>(v.base), v.len};

   auto s = self->find_stream(stream_id);
   if (!s)
      return 0;

   if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))
      s->received_headers.emplace_back(name_view, value_view);

   try
   {
      if (name_view == ":method")
         s->method = value_view;
      else if (name_view == ":path")
      {
         if (auto url = boost::urls::parse_relative_ref(value_view); url.has_value())
         {
            s->url.set_path(url->path());
            if (url->has_query())
               s->url.set_query(url->query());
            if (url->has_fragment())
               s->url.set_fragment(url->fragment());
         }
      }
      else if (name_view == ":scheme")
         s->url.set_scheme(value_view);
      else if (name_view == ":authority")
         s->url.set_encoded_authority(value_view);
      else if (name_view == "content-length")
      {
         size_t len = 0;
         if (std::from_chars(value_view.begin(), value_view.end(), len).ec == std::errc{})
            s->content_length = len;
      }
      else
         s->request_fields.set(name_view, value_view);
   }
   catch (const std::exception& ex)
   {
      logw("[{}] ignoring invalid header: {} ({})", s->log_prefix, value_view, ex.what());
   }
   return 0;
}

int Http3Session::h3_cb_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user,
                                    void*)
{
   auto self = static_cast<Http3Session*>(user);
   auto s = self->find_stream(stream_id);
   if (!s)
      return 0;

   logd("[{}] {} {}", s->log_prefix, s->method, s->url.buffer());
   log_headers(s->log_prefix, std::exchange(s->received_headers, {}));

   //
   // Build user-facing Request/Response and dispatch through the shared handler.
   //
   server::Request request(std::make_unique<Http3Reader<server::Request::Impl>>(*s));
   server::Response response(std::make_unique<Http3Writer<server::Response::Impl>>(*s));

   auto& sv = self->server_;
   if (auto& handler = sv.requestHandler())
      co_spawn(self->get_executor(), handler(std::move(request), std::move(response)), detached);
   else
   {
      loge("[{}] no request handler set", s->log_prefix);
      co_spawn(self->get_executor(), not_found(std::move(response)), detached);
   }
   return 0;
}

int Http3Session::h3_cb_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   if (auto s = self->find_stream(stream_id))
      s->on_eof();
   return 0;
}

int Http3Session::h3_cb_stop_sending(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_shutdown_stream_read(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

int Http3Session::h3_cb_reset_stream(nghttp3_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*)
{
   auto self = static_cast<Http3Session*>(user);
   ngtcp2_conn_shutdown_stream_write(self->conn_, 0, stream_id, app_error_code);
   return 0;
}

// =================================================================================================
// Server::Impl QUIC glue.
// =================================================================================================

namespace
{
std::optional<ngtcp2::Address> to_ngtcp2_address(const sockaddr_storage& src, socklen_t len)
{
   ngtcp2::Address addr{};
   if (len > sizeof(addr.su))
      return std::nullopt;
   std::memcpy(&addr.su, &src, len);
   addr.len = len;
   return addr;
}
} // namespace

void Server::Impl::associate_quic_cid(const ngtcp2_cid& cid, Http3Session* h)
{
   m_quic_handlers.emplace(cid_key(cid),
                           std::static_pointer_cast<Http3Session>(h->shared_from_this()));
}

void Server::Impl::dissociate_quic_cid(const ngtcp2_cid& cid)
{
   m_quic_handlers.erase(cid_key(cid));
}

void Server::Impl::erase_quic_session(Http3Session* h)
{
   std::erase_if(m_quic_handlers, [h](const auto& kv) { return kv.second.get() == h; });
}

// -------------------------------------------------------------------------------------------------

int Server::Impl::udp_on_read(Endpoint& ep)
{
   ngtcp2::sockaddr_union su;
   std::array<uint8_t, 64_k> buf;
   ngtcp2_pkt_info pi{};

   iovec msg_iov{buf.data(), buf.size()};
   msghdr msg{};
   msg.msg_name = &su;
   msg.msg_iov = &msg_iov;
   msg.msg_iovlen = 1;

   uint8_t
      msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(int))];
   msg.msg_control = msg_ctrl;

   //
   // Sessions that received something in this batch. They are written once, below, after every
   // datagram the socket had queued has been handed to ngtcp2 -- so one aggregate pass can pack
   // a whole response into a single GSO sendmsg() instead of dribbling it out per datagram.
   //
   std::vector<std::shared_ptr<Http3Session>> pending;
   auto mark_pending = [&pending](const std::shared_ptr<Http3Session>& session)
   {
      if (std::ranges::find(pending, session) == pending.end())
         pending.push_back(session);
   };

   for (size_t pktcnt = 0; pktcnt < 32; ++pktcnt)
   {
      if (pktcnt)
         logd("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - ", pktcnt);

      msg.msg_namelen = sizeof(su);
      msg.msg_controllen = sizeof(msg_ctrl);

      auto nread = recvmsg(ep.fd, &msg, 0);
      if (nread == -1)
      {
         if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOTCONN)
            loge("recvmsg: {}", strerror(errno));
         break; // socket drained (or broken): fall through to the write pass
      }

      if (nread < 22)
         continue;

      auto local_addr = ngtcp2::msghdr_get_local_addr(&msg, su.storage.ss_family);
      if (!local_addr)
      {
         logw("could not obtain local address from cmsg");
         continue;
      }
      ngtcp2::set_port(*local_addr, ep.addr);
      ep.addr = *local_addr;

      // When UDP_GRO is enabled the kernel may coalesce several datagrams
      // from the same 4-tuple into one recvmsg result.  The GRO cmsg carries
      // the uniform segment size so we can split the buffer back into
      // individual QUIC datagrams before handing them to ngtcp2.
      uint16_t gro_size = 0;
      for (auto* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
      {
         if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO)
         {
            memcpy(&gro_size, CMSG_DATA(cm), sizeof(gro_size));
            break;
         }
      }

      auto remote = to_ngtcp2_address(su.storage, msg.msg_namelen);
      if (!remote)
      {
         logw("unsupported remote address family");
         continue;
      }

      auto all_data = std::span<const uint8_t>{buf.data(), static_cast<size_t>(nread)};
      const size_t seg_size = gro_size > 0 ? gro_size : all_data.size();

      while (!all_data.empty())
      {
         auto data = all_data.subspan(0, std::min(seg_size, all_data.size()));
         all_data = all_data.subspan(data.size());

         ngtcp2_version_cid vc;
         auto rv = ngtcp2_pkt_decode_version_cid(&vc, data.data(), data.size(), QUIC_SCIDLEN);
         if (rv != 0)
         {
            if (rv != NGTCP2_ERR_VERSION_NEGOTIATION)
               logw("could not decode version/cid: {}", ngtcp2_strerror(rv));
            continue;
         }

         auto key = cid_key(vc.dcid, vc.dcidlen);
         auto it = m_quic_handlers.find(key);

         if (it == m_quic_handlers.end())
         {
            ngtcp2_pkt_hd hd;
            if (ngtcp2_accept(&hd, data.data(), data.size()) != 0)
               continue;

            auto session = std::make_shared<Http3Session>(*this, ep, *remote);
            if (session->init(hd.dcid, hd.scid, hd.version, pi, data) != 0)
               continue;

            m_quic_handlers.emplace(std::move(key), session);
            std::array<ngtcp2_cid, 8> scids;
            auto num_scid = ngtcp2_conn_get_scid(session->conn(), nullptr);
            if (num_scid <= scids.size())
            {
               ngtcp2_conn_get_scid(session->conn(), scids.data());
               for (size_t i = 0; i < num_scid; ++i)
                  m_quic_handlers.emplace(cid_key(scids[i]), session);
            }

            //
            // Register with the shared session set + spawn the do_session() task so
            // the session participates in server-wide shutdown, exactly like the
            // TCP-based sessions.
            //
            {
               auto lock = std::lock_guard(m_sessionMutex);
               m_sessions.emplace(session);
            }
            co_spawn(get_executor(), session->do_session({}),
                     [self = shared_from_this(), session](const std::exception_ptr& ex)
            {
               if (ex)
                  logw("[{}] {}", session->logPrefix(), what(ex));
               auto lock = std::lock_guard(self->m_sessionMutex);
               self->m_sessions.erase(session);
            });

            mark_pending(session);
         }
         else
         {
            auto session = it->second;

            //
            // Handle closing / draining periods.  During closing we resend the
            // buffered CONNECTION_CLOSE so the peer can tear down cleanly.
            // During draining (peer sent CONNECTION_CLOSE) we just drop the packet.
            // In both cases the session stays in m_quic_handlers until the 3-PTO
            // close timer fires and calls erase_quic_session().
            //
            if (auto* conn = session->conn())
            {
               if (ngtcp2_conn_in_closing_period(conn))
               {
                  session->resend_conn_close();
                  continue;
               }
               if (ngtcp2_conn_in_draining_period(conn))
                  continue;
            }

            if (session->on_read(pi, data, *remote) == 0)
            {
               mark_pending(session);
            }
            else if (session->closed())
            {
               //
               // Only erase immediately when not in closing/draining period.
               // If we are, the 3-PTO close timer in handle_error() will call
               // erase_quic_session() once the period expires.
               //
               auto* conn = session->conn();
               if (!conn ||
                   (!ngtcp2_conn_in_closing_period(conn) && !ngtcp2_conn_in_draining_period(conn)))
               {
                  std::erase_if(m_quic_handlers,
                                [&](const auto& kv) { return kv.second.get() == session.get(); });
               }
            }
         }
      }
   }

   //
   // One write pass per session, after the whole batch has been read.
   //
   for (const auto& session : pending)
   {
      if (session->flush_write() == 0 || !session->closed())
         continue;

      auto* conn = session->conn();
      if (!conn || (!ngtcp2_conn_in_closing_period(conn) && !ngtcp2_conn_in_draining_period(conn)))
         std::erase_if(m_quic_handlers,
                       [&](const auto& kv) { return kv.second.get() == session.get(); });
   }

   return 0;
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Server::Impl::udp_receive_loop()
{
   for (;;)
   {
      boost::system::error_code ec;
      co_await m_udp_socket->async_wait(boost::asio::socket_base::wait_read,
                                        redirect_error(use_awaitable, ec));
      if (ec)
      {
         if (ec == boost::asio::error::operation_aborted)
            logi("UDP receive: {}", ec.message());
         else
            logw("UDP receive: {}", ec.message());
         co_return;
      }

      Endpoint ep{};
      ep.fd = m_udp_socket->native_handle();
      auto local = m_udp_socket->local_endpoint();
      auto data = local.data();
      std::memcpy(&ep.addr.su, data, local.size());
      ep.addr.len = local.size();

      udp_on_read(ep);
   }
}

// =================================================================================================

} // namespace anyhttp::server
