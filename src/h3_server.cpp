//
// anyhttp QUIC / HTTP/3 server.
//
// Nearly everything that makes a QUIC connection work is shared with the client and lives in
// anyhttp/h3_session.hpp and anyhttp/h3_stream.hpp: `Http3ServerSession` is an
// `http3::Http3Session` that knows how packets reach it and how it dies, and `Http3ServerStream`
// is an `http3::Http3Stream` that reads a request and writes a response, where the client's does
// the opposite. Per-request state feeds an `Http3Reader` (server::Request) and `Http3Writer`
// (server::Response) into the same `RequestHandler` used by the HTTP/1.1 and HTTP/2 backends.
//
// What is genuinely server-side here: the TLS server context, the UDP receive path (many
// connections over one socket, de-multiplexed by connection ID) and the closing/draining period
// bookkeeping that goes with being the endpoint that stays around. All of it sits behind
// `Http3Server` (anyhttp/h3_backend.hpp), so the generic server in server_impl.cpp dispatches to
// HTTP/3 without ever seeing an ngtcp2 or nghttp3 type.
//
// Threading: with Config::use_strand, each Http3ServerSession lives on its own strand -- the unit
// of serialization is the QUIC *connection* (one ngtcp2_conn/nghttp3_conn pair), not the CID: many
// CIDs alias one connection. udp_receive_loop() is a single coroutine that only de-multiplexes: it
// copies each datagram, groups them by session and posts one batch per session to that session's
// strand (process_quic_batch()), where all ngtcp2/nghttp3 work, the timers and the request
// handlers run. The CID demux table is the only cross-connection state and is guarded by
// Http3ServerImpl::mutex_. Sends go straight out via a per-session dup() of the UDP fd --
// sendto()/sendmsg() are atomic per datagram, so they need no serialization.
//
// Not yet implemented: retry tokens, version negotiation, stateless reset, connection
// migration, ECN.
//

#include "anyhttp/client_impl.hpp" // IWYU pragma: keep
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3_backend.hpp"
#include "anyhttp/h3_common.hpp"
#include "anyhttp/h3_session.hpp"
#include "anyhttp/h3_stream.hpp"
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

#include <boost/container/container_fwd.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>

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
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ngtcp2/shared.h"
#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;
namespace errc = boost::system::errc;

using anyhttp::http3::log_headers;
using anyhttp::http3::make_nv;
using anyhttp::http3::QUIC_SCIDLEN;

namespace anyhttp::server
{

// =================================================================================================

struct Endpoint
{
   ngtcp2::Address addr;
   int fd;

   // Testing aid, see server::Config::drop_rate_rx/tx.
   double drop_rate_rx = 0.0;
   double drop_rate_tx = 0.0;
};

// =================================================================================================
// Free-standing helpers
// =================================================================================================

namespace
{

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

//
// Testing aid: rolls the dice for a single QUIC packet. `rate` is the probability of the packet
// being dropped, 0.0 (never, the default) to 1.0 (always). The generator is deliberately not
// seeded deterministically -- this is meant to shake out loss handling over many runs, not to
// reproduce one exact sequence.
//
bool drop_packet(double rate)
{
   if (rate <= 0.0)
      return false;

   static thread_local std::mt19937 rng{std::random_device{}()};
   return std::uniform_real_distribution<double>{0.0, 1.0}(rng) < rate;
}

// -------------------------------------------------------------------------------------------------

int send_udp(const Endpoint& ep, const sockaddr* sa, socklen_t salen, std::span<const uint8_t> data)
{
   if (drop_packet(ep.drop_rate_tx))
   {
      // logw("*** dropping outgoing packet ({} bytes) ***", data.size());
      return 0; // pretend it went out; ngtcp2 will retransmit
   }

   for (;;)
   {
      auto n = ::sendto(ep.fd, data.data(), data.size(), 0, sa, salen);
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
int send_udp_gso(const Endpoint& ep, const sockaddr* sa, socklen_t salen,
                 std::span<const uint8_t> data, size_t gso_size, bool& no_gso)
{
   // With TX dropping enabled, go packet by packet so each one can be dropped individually.
   if (no_gso || data.size() <= gso_size || ep.drop_rate_tx > 0.0)
   {
      for (; !data.empty();)
      {
         auto len = std::min(gso_size, data.size());
         if (send_udp(ep, sa, salen, data.first(len)) != 0)
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
      auto n = ::sendmsg(ep.fd, &msg, 0);
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
            return send_udp_gso(ep, sa, salen, data, gso_size, no_gso);
         }
         loge("sendmsg (GSO): {}", strerror(errno));
         return -1;
      }
      return 0;
   }
}

} // namespace

// =================================================================================================
// Http3ServerStream / Http3ServerSession: the server's end of the shared HTTP/3 implementation.
// =================================================================================================

class Http3ServerImpl;
class Http3ServerSession;

class Http3ServerStream : public http3::Http3Stream
{
public:
   Http3ServerStream(Http3ServerSession& session, int64_t id);

   //
   // Reads a request, writes a response -- the mirror image of Http3ClientStream. The response
   // body is handed to nghttp3 by reference (WriteMode::ZeroCopy): a file served through
   // serve_file() travels from the page cache into QUIC packets without an intermediate copy.
   //
   void on_pseudo_header(std::string_view name, std::string_view value) override;
   void on_headers_complete() override;
   void submit_response(unsigned int status_code, const Fields& fields) override;
};

// -------------------------------------------------------------------------------------------------

class Http3ServerSession : public http3::Http3Session
{
public:
   Http3ServerSession(Http3ServerImpl& server, Endpoint ep, ngtcp2::Address remote);
   ~Http3ServerSession() override;

   //
   // Session::Impl
   //
   // The executor is this connection's strand (when Config::use_strand is set): every touch of
   // ngtcp2/nghttp3 state -- datagram batches from the UDP demux, the expiry timer, wake_write()
   // flushes, and the request-handler coroutines spawned in on_headers_complete() -- runs through
   // it, giving one QUIC connection the same single-threaded world a TCP connection gets from
   // the strand its socket lives on.
   //
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

   /// Called from Http3ServerImpl::process_quic_batch() when a packet arrives during the
   /// closing period.
   void resend_conn_close();

   const ngtcp2_cid& scid() const noexcept { return scid_; }
   Http3ServerImpl& server() noexcept { return server_; }

protected:
   int handle_error(int rv) override;
   int send_datagrams(const ngtcp2_path& path, std::span<const uint8_t> data,
                      size_t gso_size) override;
   std::shared_ptr<http3::Http3Stream> make_stream(int64_t id) override;
   void on_new_cid(const ngtcp2_cid& cid) override;
   void on_remove_cid(const ngtcp2_cid& cid) override;

private:
   void signal_done();
   void schedule_close_timer();
   void do_destroy() noexcept; // the body of destroy(), always run on executor_

private:
   Http3ServerImpl& server_;
   Endpoint ep_;
   bool owns_fd_ = false; // ep_.fd was dup()ed in the ctor, close it in the dtor
   ngtcp2::Address remote_;
   ngtcp2_cid scid_{};

   asio::steady_timer done_signal_; // used to wake do_session() on connection close
   std::vector<uint8_t> conn_closebuf_; // buffered CONNECTION_CLOSE packet
   bool no_gso_ = false;
};

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

//
// What one pass of udp_on_read() hands a session: every datagram of the receive batch that was
// addressed to it, copied out of the receive buffer because the session consumes them on its own
// strand, after udp_on_read() has moved on. `is_new` marks a batch whose first datagram is the
// client Initial that created the session -- process_quic_batch() runs init() with it.
//
struct QuicBatch
{
   struct Datagram
   {
      ngtcp2_pkt_info pi;
      ngtcp2::Address remote;
      std::vector<uint8_t> data;
   };

   bool is_new = false;
   ngtcp2_pkt_hd hd{}; // decoded Initial packet header, only valid when is_new
   boost::container::small_vector<Datagram, 8> datagrams;
};

// -------------------------------------------------------------------------------------------------

//
// The server's HTTP/3 half (see anyhttp/h3_backend.hpp): the UDP socket every QUIC connection
// shares, the receive loop de-multiplexing datagrams onto them by connection ID, and the table
// doing that lookup. The sessions themselves are owned by Server::Impl's session registry, like
// the TCP-based ones -- what is kept here is only what routing packets needs.
//
class Http3ServerImpl : public Http3Server, public std::enable_shared_from_this<Http3ServerImpl>
{
public:
   Http3ServerImpl(Server::Impl& parent, const asio::ip::udp::endpoint& endpoint);

   //
   // Http3Server
   //
   void start() override;
   void destroy() override;

   //
   // The server this is a part of. Sessions reach the configuration, the request handler and the
   // session registry through here.
   //
   Server::Impl& parent() noexcept { return parent_; }
   const Config& config() const noexcept { return parent_.config(); }
   const RequestHandler& requestHandler() const { return parent_.requestHandler(); }
   asio::any_io_executor get_executor() const noexcept { return parent_.get_executor(); }

   //
   // QUIC connection-ID demux table. Populated as new source CIDs are minted, consulted by
   // udp_on_read() to route packets to the right connection. Guarded by mutex_: the receive loop
   // reads it while sessions mutate it from their own strands (get_new_connection_id /
   // remove_connection_id callbacks, close timers).
   //
   void associate_quic_cid(const ngtcp2_cid& cid, Http3ServerSession* session);
   void dissociate_quic_cid(const ngtcp2_cid& cid);
   void erase_quic_session(Http3ServerSession* session);

private:
   awaitable<void> udp_receive_loop();
   int udp_on_read(Endpoint& ep);
   void process_quic_batch(const std::shared_ptr<Http3ServerSession>& session, QuicBatch&& batch);

   /// Keeps the owning Server::Impl alive for as long as a pending operation of ours runs.
   std::shared_ptr<Server::Impl> owner() { return parent_.shared_from_this(); }

private:
   Server::Impl& parent_;

   //
   // The socket gets its own strand: udp_receive_loop() runs on it, and destroy() dispatches the
   // shutdown close() through it, so the two never touch the socket concurrently.
   //
   std::optional<asio::ip::udp::socket> socket_;

   std::mutex mutex_;
   std::unordered_map<std::string, std::shared_ptr<Http3ServerSession>> sessions_;
};

// =================================================================================================
// Http3ServerStream implementation
// =================================================================================================

Http3ServerStream::Http3ServerStream(Http3ServerSession& s, int64_t stream_id)
   : http3::Http3Stream(s, stream_id, http3::WriteMode::ZeroCopy)
{
}

void Http3ServerStream::on_pseudo_header(std::string_view name, std::string_view value)
{
   if (name == ":method")
      method = value;
   else if (name == ":path")
   {
      if (auto parsed = boost::urls::parse_relative_ref(value); parsed.has_value())
      {
         url.set_path(parsed->path());
         if (parsed->has_query())
            url.set_query(parsed->query());
         if (parsed->has_fragment())
            url.set_fragment(parsed->fragment());
      }
   }
   else if (name == ":scheme")
      url.set_scheme(value);
   else if (name == ":authority")
      url.set_encoded_authority(value);
}

void Http3ServerStream::on_headers_complete()
{
   logd("[{}] {} {}", log_prefix, method, url.buffer());
   log_headers(log_prefix, std::exchange(received_headers, {}));

   //
   // Build the user-facing Request/Response and dispatch through the shared handler.
   //
   server::Request request(std::make_unique<http3::Http3Reader<server::Request::Impl>>(*this));
   server::Response response(std::make_unique<http3::Http3Writer<server::Response::Impl>>(*this));

   auto& sv = static_cast<Http3ServerSession&>(session).server();
   if (auto& handler = sv.requestHandler())
      co_spawn(get_executor(), handler(std::move(request), std::move(response)), detached);
   else
   {
      loge("[{}] no request handler set", log_prefix);
      co_spawn(get_executor(), not_found(std::move(response)), detached);
   }
}

void Http3ServerStream::submit_response(unsigned int status, const Fields& user_fields)
{
   assert(!headers_submitted);

   response_status = status;
   response_fields = user_fields;

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

   using namespace boost::beast::http;
   logd("[{}] {} {}", log_prefix, response_status, obsolete_reason(int_to_status(response_status)));

   if (submit_headers(nva, false /* response */))
      session.wake_write();
}

// =================================================================================================
// Http3ServerSession implementation
// =================================================================================================

Http3ServerSession::Http3ServerSession(Http3ServerImpl& server, Endpoint ep, ngtcp2::Address remote)
   : http3::Http3Session(server.config().use_strand
                            ? asio::any_io_executor{asio::make_strand(server.get_executor())}
                            : server.get_executor()),
     server_(server), ep_(ep), remote_(remote), done_signal_(get_executor())
{
   log_prefix_ = std::format("h3:{}", ngtcp2::util::straddr(&remote_.su.sa, remote_.len));

   //
   // Own a dup() of the shared UDP fd rather than borrowing the server's. Sends happen from this
   // session's strand, concurrently with everything else -- sendto()/sendmsg() on a shared
   // datagram fd is fine, each call is atomic -- but at shutdown the server closes its socket
   // right after posting destroy() to every session, and the final CONNECTION_CLOSE would
   // otherwise race that close (and, worse, a recycled fd number).
   //
   if (int fd = ::dup(ep_.fd); fd >= 0)
   {
      ep_.fd = fd;
      owns_fd_ = true;
   }
   else
      loge("[{}] dup: {}", log_prefix_, strerror(errno));

   // done_signal_ is armed at "never" until signal_done() moves it to the past.
   done_signal_.expires_at(asio::steady_timer::time_point::max());
   mlogd("session created");
}

Http3ServerSession::~Http3ServerSession()
{
   //
   // Tear the streams down while this object is still whole: destroying a stream fires pending
   // handlers, which reach back into the session.
   //
   done_signal_.cancel();
   clear_streams();
   if (owns_fd_)
      ::close(ep_.fd);
   mlogi("session destroyed");
}

// -------------------------------------------------------------------------------------------------

void Http3ServerSession::async_submit(SubmitHandler&& handler, boost::urls::url, const Fields&)
{
   // A server does not initiate requests; see Http3ClientSession::async_submit().
   std::move(handler)(errc::make_error_code(errc::operation_not_supported),
                      client::Request{nullptr});
}

awaitable<void> Http3ServerSession::do_session(Buffer&&)
{
   boost::system::error_code ec;
   co_await done_signal_.async_wait(redirect_error(use_awaitable, ec));
   // ec is boost::asio::error::operation_aborted (from destroy()) or a spurious
   // wake-up; either way, this coroutine's job is done.
   co_return;
}

void Http3ServerSession::destroy() noexcept
{
   //
   // Called from wherever the Server is being torn down -- under multithreading that is some
   // other thread's strand (e.g. the signal handler in server_main), while this session's own
   // strand may be mid-flush. Everything below touches ngtcp2 state and the timers, so hop onto
   // this session's executor first; with use_strand off and the caller already inside the
   // io_context, dispatch() degenerates to an inline call.
   //
   asio::dispatch(get_executor(), [self = shared_from_this()]
   { static_cast<Http3ServerSession&>(*self).do_destroy(); });
}

void Http3ServerSession::do_destroy() noexcept
{
   //
   // Tear the streams down here, on the session's executor, rather than leaving it to the
   // destructor. The destructor runs wherever the last shared_ptr happens to drop -- at server
   // shutdown that can be Server::Impl::~Impl() on a foreign thread -- and destroying streams
   // detaches readers/writers and fires pending handlers, state that request-handler coroutines
   // still running on this session's strand look at. Done here, that teardown is serialized
   // with them; a handler that resumes afterwards finds its Reader/Writer detached and fails
   // cleanly, exactly as in the single-threaded case.
   //
   clear_streams();

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
      if (auto packet = write_connection_close(closebuf, ps); !packet.empty())
         send_udp(ep_, ps.path.remote.addr, ps.path.remote.addrlen, packet);
   }

   timer_.cancel();
   signal_done();
}

void Http3ServerSession::signal_done()
{
   // Move the sentinel timer to the past so any waiter wakes up.
   done_signal_.expires_at(asio::steady_timer::time_point::min());
}

// -------------------------------------------------------------------------------------------------

std::shared_ptr<http3::Http3Stream> Http3ServerSession::make_stream(int64_t id)
{
   return std::make_shared<Http3ServerStream>(*this, id);
}

void Http3ServerSession::on_new_cid(const ngtcp2_cid& cid)
{
   server_.associate_quic_cid(cid, this);
}

void Http3ServerSession::on_remove_cid(const ngtcp2_cid& cid) { server_.dissociate_quic_cid(cid); }

int Http3ServerSession::send_datagrams(const ngtcp2_path& path, std::span<const uint8_t> data,
                                       size_t gso_size)
{
   return send_udp_gso(ep_, path.remote.addr, path.remote.addrlen, data, gso_size, no_gso_);
}

// -------------------------------------------------------------------------------------------------

int Http3ServerSession::init(const ngtcp2_cid& dcid, const ngtcp2_cid& scid, uint32_t version,
                             const ngtcp2_pkt_info& pi, std::span<const uint8_t> data)
{
   scid_.datalen = QUIC_SCIDLEN;
   if (RAND_bytes(scid_.data, static_cast<int>(scid_.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for SCID failed", log_prefix_);
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   fill_callbacks(callbacks);
   callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;

   ngtcp2_settings settings;
   ngtcp2_transport_params params;
   fill_settings(settings, params, server_.config().idle_timeout);
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

   if (setup_tls(tls_context().ctx, true /* server */) != 0)
      return -1;

   logi("[{}] new connection, scid={} version=0x{:x}", log_prefix_,
        ngtcp2::util::format_hex(scid_.data, scid_.datalen), version);

   return on_read(pi, data, remote_);
}

// -------------------------------------------------------------------------------------------------

int Http3ServerSession::on_read(const ngtcp2_pkt_info& pi, std::span<const uint8_t> data,
                                const ngtcp2::Address& remote)
{
   ngtcp2_path path{
      {const_cast<sockaddr*>(&ep_.addr.su.sa), ep_.addr.len},
      {const_cast<sockaddr*>(&remote.su.sa), remote.len},
      &ep_,
   };

   return http3::Http3Session::on_read(path, pi, data);
}

// -------------------------------------------------------------------------------------------------

int Http3ServerSession::handle_error(int /*rv*/)
{
   if (closed_)
      return -1;
   closed_ = true;

   //
   // Idle timeout and drop-conn need no CONNECTION_CLOSE packet -- and with no packet there is
   // no closing period either, so none of the cleanup in Http3ServerImpl::process_quic_batch() can
   // ever run for this session: it is reached from the expiry timer precisely because nothing is
   // arriving any more. Drop the session from the demux map right here instead, or it would sit
   // in sessions_ for the lifetime of the server, holding streams whose request handlers
   // are still waiting on a peer that went away. What is left of it then dies with do_session().
   //
   if (last_error_.type == NGTCP2_CCERR_TYPE_IDLE_CLOSE ||
       last_error_.type == NGTCP2_CCERR_TYPE_DROP_CONN)
   {
      auto self = weak_from_this().lock(); // erase_quic_session() may drop the last reference
      timer_.cancel();
      server_.erase_quic_session(this);
      signal_done();
      return -1;
   }

   // If already in draining (peer sent CONNECTION_CLOSE), don't reply.
   // If already in closing, the buffered packet is still valid.
   if (conn_ && !ngtcp2_conn_in_draining_period(conn_) && !ngtcp2_conn_in_closing_period(conn_))
   {
      conn_closebuf_.resize(NGTCP2_MAX_UDP_PAYLOAD_SIZE);
      ngtcp2_path_storage ps;
      auto packet = write_connection_close(conn_closebuf_, ps);
      if (!packet.empty())
      {
         conn_closebuf_.resize(packet.size());
         logi("[{}] sending CONNECTION_CLOSE", log_prefix_);
         send_udp(ep_, ps.path.remote.addr, ps.path.remote.addrlen, conn_closebuf_);
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

void Http3ServerSession::schedule_close_timer()
{
   auto delay = conn_ ? std::chrono::nanoseconds{ngtcp2_conn_get_pto(conn_) * 3}
                      : std::chrono::nanoseconds{std::chrono::milliseconds{100}};
   timer_.expires_after(delay);
   timer_.async_wait([self = weak_from_this()](const boost::system::error_code& ec)
   {
      if (ec)
         return;
      auto session = std::static_pointer_cast<Http3ServerSession>(self.lock());
      if (!session)
         return;
      logd("[{}] closing/draining period over", session->logPrefix());
      session->server_.erase_quic_session(session.get());
      session->signal_done();
   });
}

void Http3ServerSession::resend_conn_close()
{
   if (conn_closebuf_.empty())
      return;
   auto* path = ngtcp2_conn_get_path(conn_);
   if (!path)
      return;
   logd("[{}] resending CONNECTION_CLOSE", log_prefix_);
   send_udp(ep_, path->remote.addr, path->remote.addrlen, conn_closebuf_);
}

// =================================================================================================
// Http3ServerImpl: the UDP socket, the receive loop and the connection-ID demux.
// =================================================================================================

Http3ServerImpl::Http3ServerImpl(Server::Impl& parent, const asio::ip::udp::endpoint& endpoint)
   : parent_(parent)
{
   namespace socket_option = boost::asio::detail::socket_option;

   const bool is_v6 = endpoint.protocol() == ip::udp::v6();

   socket_.emplace(config().use_strand ? asio::make_strand(parent_.get_executor())
                                       : parent_.get_executor());
   socket_->open(is_v6 ? ip::udp::v6() : ip::udp::v4());

   if (is_v6)
   {
      boost::system::error_code ec;
      socket_->set_option(ip::v6_only(false), ec);
      socket_->set_option(socket_option::integer<IPPROTO_IPV6, IPV6_RECVTCLASS>(1));
      socket_->set_option(socket_option::integer<IPPROTO_IPV6, IPV6_MTU_DISCOVER>(1));
      socket_->set_option(socket_option::integer<IPPROTO_IPV6, IPV6_RECVPKTINFO>(1));
   }
   else
   {
      socket_->set_option(socket_option::integer<IPPROTO_IP, IP_RECVTOS>(1));
      socket_->set_option(socket_option::integer<IPPROTO_IP, IP_PKTINFO>(1));
   }
   socket_->set_option(socket_option::integer<IPPROTO_UDP, UDP_GRO>(1));
   socket_->non_blocking(true);

   socket_->bind(endpoint);
   logi("Server: UDP listening on {}", endpoint);
}

// -------------------------------------------------------------------------------------------------

void Http3ServerImpl::start()
{
   // On the socket's strand, so that the loop and destroy()'s close() never race on the socket.
   co_spawn(socket_->get_executor(), udp_receive_loop(),
            [self = shared_from_this(), owner = owner()](const std::exception_ptr& ex)
   {
      if (ex)
         logw("UDP receive loop: {}", what(ex));
      else
         logi("UDP receive loop: done");
   });
}

void Http3ServerImpl::destroy()
{
   //
   // The socket lives on its own strand and udp_receive_loop() keeps re-arming async_wait() on
   // it there -- asio sockets are not thread-safe, so the close has to go through the same
   // strand instead of racing that from here. The QUIC sessions have already been destroyed by
   // Server::Impl at this point, each sending its final CONNECTION_CLOSE through its own
   // dup()ed fd, so closing this socket doesn't race that.
   //
   asio::dispatch(socket_->get_executor(), [self = shared_from_this(), owner = owner()]
   { self->socket_->close(); }); // breaks udp_receive_loop()
}

// -------------------------------------------------------------------------------------------------

void Http3ServerImpl::associate_quic_cid(const ngtcp2_cid& cid, Http3ServerSession* h)
{
   auto lock = std::lock_guard(mutex_);
   sessions_.emplace(cid_key(cid),
                     std::static_pointer_cast<Http3ServerSession>(h->shared_from_this()));
}

void Http3ServerImpl::dissociate_quic_cid(const ngtcp2_cid& cid)
{
   auto lock = std::lock_guard(mutex_);
   sessions_.erase(cid_key(cid));
}

void Http3ServerImpl::erase_quic_session(Http3ServerSession* h)
{
   auto lock = std::lock_guard(mutex_);
   std::erase_if(sessions_, [h](const auto& kv) { return kv.second.get() == h; });
}

// -------------------------------------------------------------------------------------------------

int Http3ServerImpl::udp_on_read(Endpoint& ep)
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
   // Datagrams collected per session over the whole batch. Each session gets its accumulated
   // batch posted to its strand once, below, after every datagram the socket had queued has been
   // de-multiplexed -- so one aggregate pass on the strand can pack a whole response into a
   // single GSO sendmsg() instead of dribbling it out per datagram.
   //
   boost::container::small_flat_map<std::shared_ptr<Http3ServerSession>, QuicBatch, 32> batches;
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

      for (size_t segcnt = 0; !all_data.empty(); ++segcnt)
      {
         if (segcnt)
            logd("-   {}     -   -   -   -   -   -   -   -   -   -   -   -   -   -   -", segcnt);

         auto data = all_data.subspan(0, std::min(seg_size, all_data.size()));
         all_data = all_data.subspan(data.size());

         if (drop_packet(ep.drop_rate_rx))
         {
            // logw("*** dropping received packet ({} bytes) ***", data.size());
            continue;
         }

         ngtcp2_version_cid vc;
         auto rv = ngtcp2_pkt_decode_version_cid(&vc, data.data(), data.size(), QUIC_SCIDLEN);
         if (rv != 0)
         {
            if (rv != NGTCP2_ERR_VERSION_NEGOTIATION)
               logw("could not decode version/cid: {}", ngtcp2_strerror(rv));
            continue;
         }

         auto key = cid_key(vc.dcid, vc.dcidlen);
         std::shared_ptr<Http3ServerSession> session;
         {
            auto lock = std::lock_guard(mutex_);
            if (auto it = sessions_.find(key); it != sessions_.end())
               session = it->second;
         }

         if (!session)
         {
            ngtcp2_pkt_hd hd;
            if (ngtcp2_accept(&hd, data.data(), data.size()) != 0)
               continue;

            session = std::make_shared<Http3ServerSession>(*this, ep, *remote);

            //
            // Publish the client-chosen DCID right away, so retransmitted Initials and
            // follow-up packets -- in this batch or a later one -- find the session and queue
            // up behind init() on its strand instead of spawning a duplicate session. init()
            // itself, like everything that touches the connection, runs on the strand in
            // process_quic_batch().
            //
            auto lock = std::lock_guard(mutex_);
            sessions_.emplace(std::move(key), session);
            auto& batch = batches[session];
            batch.is_new = true;
            batch.hd = hd;
         }

         batches[session].datagrams.push_back({pi, *remote, {data.begin(), data.end()}});
      }
   }

   //
   // One job per session: its whole share of the receive batch, processed -- and answered with
   // a single write pass -- on its own strand.
   //
   for (auto& [session, batch] : batches)
   {
      asio::post(session->get_executor(),
                 [self = shared_from_this(), owner = owner(), session,
                  batch = std::move(batch)]() mutable { // 
          self->process_quic_batch(session, std::move(batch));
      });
   }

   return 0;
}

// -------------------------------------------------------------------------------------------------

//
// Runs on the session's strand: consumes the datagrams udp_on_read() collected for this session,
// serialized against the session's timers, wake_write() flushes and request handlers. This is
// what the demux loop used to do inline back when everything shared one implicit thread.
//
void Http3ServerImpl::process_quic_batch(const std::shared_ptr<Http3ServerSession>& session,
                                         QuicBatch&& batch)
{
   size_t next = 0;
   bool read_ok = false;

   if (batch.is_new)
   {
      const auto& first = batch.datagrams[next++];
      if (session->init(batch.hd.dcid, batch.hd.scid, batch.hd.version, first.pi, first.data) != 0)
      {
         // Matches the old inline behavior: a connection that failed at its Initial is
         // forgotten; a retransmitted Initial starts over from scratch.
         erase_quic_session(session.get());
         return;
      }
      read_ok = true;

      std::array<ngtcp2_cid, 8> scids;
      auto num_scid = ngtcp2_conn_get_scid(session->conn(), nullptr);
      if (num_scid <= scids.size())
      {
         ngtcp2_conn_get_scid(session->conn(), scids.data());
         for (size_t i = 0; i < num_scid; ++i)
            associate_quic_cid(scids[i], session.get());
      }

      //
      // Register with the server's session registry + spawn the do_session() task so the
      // session participates in server-wide shutdown, exactly like the TCP-based ones.
      // Registration fails if the server was destroyed between udp_on_read() accepting this
      // connection and this job running; the session is then ours to tear down.
      //
      if (!parent_.add_session(session))
      {
         erase_quic_session(session.get());
         session->destroy();
         return;
      }

      co_spawn(session->get_executor(), session->do_session({}),
               [self = shared_from_this(), owner = owner(), session](const std::exception_ptr& ex)
      {
         if (ex)
            logw("[{}] {}", session->logPrefix(), what(ex));
         self->parent_.remove_session(session);
      });
   }

   for (; next < batch.datagrams.size(); ++next)
   {
      const auto& d = batch.datagrams[next];

      //
      // Handle closing / draining periods.  During closing we resend the
      // buffered CONNECTION_CLOSE so the peer can tear down cleanly.
      // During draining (peer sent CONNECTION_CLOSE) we just drop the packet.
      // In both cases the session stays in sessions_ until the 3-PTO
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

      // A session that died earlier in this very batch has nothing left to feed.
      if (session->closed())
         continue;

      if (session->on_read(d.pi, d.data, d.remote) == 0)
      {
         read_ok = true;
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
            erase_quic_session(session.get());
      }
   }

   //
   // One write pass for the whole batch, mirroring the old per-receive-pass flush.
   //
   if (!read_ok)
      return;
   if (session->flush_write() == 0 || !session->closed())
      return;

   auto* conn = session->conn();
   if (!conn || (!ngtcp2_conn_in_closing_period(conn) && !ngtcp2_conn_in_draining_period(conn)))
      erase_quic_session(session.get());
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Http3ServerImpl::udp_receive_loop()
{
   for (;;)
   {
      boost::system::error_code ec;
      co_await socket_->async_wait(boost::asio::socket_base::wait_read,
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
      ep.fd = socket_->native_handle();
      ep.drop_rate_rx = config().drop_rate_rx;
      ep.drop_rate_tx = config().drop_rate_tx;
      auto local = socket_->local_endpoint();
      auto data = local.data();
      std::memcpy(&ep.addr.su, data, local.size());
      ep.addr.len = local.size();

      udp_on_read(ep);
   }
}

// =================================================================================================

std::shared_ptr<Http3Server> make_http3_server(Server::Impl& server,
                                               const asio::ip::udp::endpoint& endpoint)
{
   return std::make_shared<Http3ServerImpl>(server, endpoint);
}

// =================================================================================================

} // namespace anyhttp::server
