//
// anyhttp QUIC / HTTP/3 client.
//
// Almost all of it is shared with the server: `Http3ClientSession` is an `http3::Http3Session`
// (see anyhttp/h3_session.hpp) that knows how packets reach it and how it is torn down, and
// `Http3ClientStream` is an `http3::Http3Stream` (anyhttp/h3_stream.hpp) that writes a request
// and reads a response, where the server's does the opposite.
//
// What is genuinely client-side here: the TLS client context, one `connect()`ed UDP socket per
// session -- there is exactly one peer, so no connection-ID demux table is needed, unlike the
// server's shared socket -- the receive loop feeding it, and async_submit(), which opens a stream
// and puts the request headers on it before handing a client::Request back to the caller.
//
// Not yet implemented: certificate verification, 0-RTT, connection migration, GSO/ECN, retry
// tokens, graceful (multi-PTO) close.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3_backend.hpp"
#include "anyhttp/h3_common.hpp"
#include "anyhttp/h3_session.hpp"
#include "anyhttp/h3_stream.hpp"
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
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;
namespace errc = boost::system::errc;

using anyhttp::http3::log_headers;
using anyhttp::http3::make_nv;

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
      // anything.
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

} // namespace

// =================================================================================================
// Http3ClientStream / Http3ClientSession: the client's end of the shared HTTP/3 implementation.
// =================================================================================================

class Http3ClientSession;

class Http3ClientStream : public http3::Http3Stream
{
public:
   Http3ClientStream(Http3ClientSession& session, int64_t id);
   ~Http3ClientStream() override;

   //
   // Writes a request, reads a response -- the mirror image of Http3ServerStream. The request
   // body goes through the staging buffer (WriteMode::Staged) rather than being handed to nghttp3
   // by reference: that keeps cancellation instantaneous and, more importantly, leaves the stream
   // intact afterwards, so a cancelled write can be followed by another one on the same request.
   //
   void on_pseudo_header(std::string_view name, std::string_view value) override;
   void on_headers_complete() override;
   void on_failed(boost::system::error_code ec) override;

   /// The request headers went out with the stream itself, see Http3ClientSession::async_submit().
   void submit_response(unsigned int, const Fields&) override {}

   /// Assembles and submits the request headers. Called once, right after the stream is created.
   bool submit_request(const boost::urls::url& url, const Fields& headers);

   void async_get_response(client::Request::GetResponseHandler&& handler);
   void deliver_response();
   void deliver_failure();

   bool response_delivered = false;
   client::Request::GetResponseHandler response_handler;

   //
   // Why the stream died, remembered because async_get_response() may well be called only
   // afterwards -- a response that can never arrive must not leave its caller waiting forever.
   //
   boost::system::error_code failure_ec;
};

// -------------------------------------------------------------------------------------------------

//
// The client's Request needs one thing the shared writer does not have: async_get_response(),
// which client.hpp exposes and server::Response has no counterpart for.
//
class Http3ClientWriter : public http3::Http3Writer<client::Request::Impl>
{
public:
   using http3::Http3Writer<client::Request::Impl>::Http3Writer;

   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      if (!stream)
      {
         std::move(handler)(errc::make_error_code(errc::connection_aborted),
                            client::Response{nullptr});
         return;
      }
      static_cast<Http3ClientStream*>(stream)->async_get_response(std::move(handler));
   }
};

// -------------------------------------------------------------------------------------------------

class Http3ClientSession : public http3::Http3Session
{
public:
   explicit Http3ClientSession(asio::any_io_executor executor);
   ~Http3ClientSession() override;

   //
   // Session::Impl
   //
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
   bool ready() const noexcept { return h3() != nullptr; }

protected:
   int handle_error(int rv) override;
   int send_datagrams(const ngtcp2_path& path, std::span<const uint8_t> data,
                      size_t gso_size) override;
   std::shared_ptr<http3::Http3Stream> make_stream(int64_t id) override;
   void on_http3_ready() override { signal_ready(); }

private:
   int on_read(std::span<const uint8_t> data);
   void close();
   void signal_ready();

private:
   asio::ip::udp::socket socket_;
   asio::steady_timer ready_signal_; // sentinel timer, see wait_ready()
};

// =================================================================================================
// Http3ClientStream implementation
// =================================================================================================

Http3ClientStream::Http3ClientStream(Http3ClientSession& s, int64_t stream_id)
   : http3::Http3Stream(s, stream_id, http3::WriteMode::Staged)
{
}

Http3ClientStream::~Http3ClientStream()
{
   if (!response_delivered && response_handler)
      swap_and_invoke(response_handler, errc::make_error_code(errc::connection_reset),
                      client::Response{nullptr});
}

void Http3ClientStream::on_pseudo_header(std::string_view name, std::string_view value)
{
   if (name == ":status")
   {
      unsigned int status = 0;
      if (std::from_chars(value.begin(), value.end(), status).ec == std::errc{})
         status_code = status;
   }
}

void Http3ClientStream::on_headers_complete()
{
   logd("[{}] response headers: status={}", log_prefix, status_code);
   log_headers(log_prefix, std::exchange(received_headers, {}));
   deliver_response();
}

void Http3ClientStream::on_failed(boost::system::error_code ec)
{
   //
   // A stream closing gracefully (ec success, e.g. NGHTTP3_H3_NO_ERROR) still means no response
   // ever arrived if headers were never received -- never report success with a null Response.
   //
   failure_ec = ec ? ec : boost::beast::http::error::end_of_stream;
   deliver_failure();
}

void Http3ClientStream::deliver_failure()
{
   if (headers_received || response_delivered || !response_handler)
      return;

   response_delivered = true;
   swap_and_invoke(response_handler, failure_ec, client::Response{nullptr});
}

bool Http3ClientStream::submit_request(const boost::urls::url& request_url, const Fields& headers)
{
   url = request_url;

   //
   // TODO: CONNECT / other methods -- mirrors the h2 client's NGHttp2Session::async_submit(),
   // which is likewise hard-coded to POST.
   //
   std::string method_str("POST");
   std::string scheme(request_url.scheme());
   std::string target(request_url.encoded_target());
   std::string authority(request_url.host_address());

   std::vector<nghttp3_nv> nva;
   nva.reserve(16); // small typical header count; vector will grow if needed
   nva.push_back(make_nv(":method", method_str));
   nva.push_back(make_nv(":scheme", scheme));
   nva.push_back(make_nv(":path", target));
   nva.push_back(make_nv(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}': setting pseudo headers is not allowed",
              log_prefix, item.name_string());
      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   return submit_headers(nva, true /* request */);
}

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

   //
   // Delivering the response resumes the caller, which may drop the last reference to this stream
   // right there (its Request going out of scope with the Response never read). Keep it alive
   // until this function returns.
   //
   auto self = shared_from_this();

   response_handler = std::move(handler);
   deliver_response();

   //
   // Nothing will ever arrive on a stream that is already dead, so answer right away instead of
   // waiting for a response that cannot come -- the same reasoning that makes call_read_handler()
   // report the truncation to a read issued after the close.
   //
   if (closed)
      deliver_failure();
}

void Http3ClientStream::deliver_response()
{
   if (!headers_received || !response_handler)
      return;

   response_delivered = true;
   auto response =
      client::Response{std::make_unique<http3::Http3Reader<client::Response::Impl>>(*this)};
   swap_and_invoke(response_handler, boost::system::error_code{}, std::move(response));
}

// =================================================================================================
// Http3ClientSession implementation
// =================================================================================================

Http3ClientSession::Http3ClientSession(asio::any_io_executor executor)
   : http3::Http3Session(executor), socket_(get_executor()), ready_signal_(get_executor())
{
   // Sentinel timers: expires_at(max) means "not yet"; a wait completes once moved to "min".
   ready_signal_.expires_at(asio::steady_timer::time_point::max());
   logi("Http3ClientSession: ctor");
}

Http3ClientSession::~Http3ClientSession()
{
   //
   // Tear the streams down while this object is still whole: destroying a stream fires pending
   // handlers, which reach back into the session.
   //
   ready_signal_.cancel();
   clear_streams();
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

   log_prefix_ = std::format("h3:{}", ngtcp2::util::straddr(remote.data(), remote.size()));

   ngtcp2_cid scid{};
   scid.datalen = 17;
   if (RAND_bytes(scid.data, static_cast<int>(scid.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for SCID failed", log_prefix_);
      return -1;
   }
   ngtcp2_cid dcid{};
   dcid.datalen = http3::QUIC_SCIDLEN;
   if (RAND_bytes(dcid.data, static_cast<int>(dcid.datalen)) != 1)
   {
      loge("[{}] init: RAND_bytes for DCID failed", log_prefix_);
      return -1;
   }

   ngtcp2_callbacks callbacks{};
   fill_callbacks(callbacks);
   callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
   callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;

   ngtcp2_settings settings;
   ngtcp2_transport_params params;
   fill_settings(settings, params, 30s);

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

   if (setup_tls(tls_context().ctx, false /* client */) != 0)
      return -1;

   logi("[{}] connecting, scid={}", log_prefix_, ngtcp2::util::format_hex(scid.data, scid.datalen));
   return 0;
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Http3ClientSession::do_session(Buffer&&)
{
   if (flush_write() != 0)
   {
      signal_ready();
      co_return;
   }

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

      //
      // close() may have run from inside on_read(): handing a response chunk or EOF to the
      // application resumes its coroutine, which may drop the last reference to the Session
      // right there. Its socket_.cancel() then found no receive pending -- we are between two
      // of them -- so nothing would stop us from arming a fresh one that no peer will ever
      // complete. The server, already draining because it got our CONNECTION_CLOSE, does not
      // even answer it.
      //
      if (closed())
         break;
   }

   //
   // The receive loop only ever ends because this connection is over: the socket errored out (ICMP
   // reporting the peer's port unreachable, say), close() cancelled it, or on_read() hit a protocol
   // error. Tear the session down in every case -- nothing else is running that could ever complete
   // the requests still waiting on it, so leaving them pending hangs them forever. close() is
   // idempotent, so the paths that already tore things down are unaffected, and it signals ready to
   // unblock a waiter whose handshake never finished.
   //
   close();
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
   std::vector<std::shared_ptr<http3::Http3Stream>> streams;
   streams.reserve(streams_.size());
   for (auto& [id, stream] : streams_)
      streams.push_back(stream);
   for (auto& stream : streams)
      stream->fail(errc::make_error_code(errc::connection_reset));

   //
   // An idle-timed-out (or dropped) connection is discarded silently: RFC 9000 has no
   // CONNECTION_CLOSE for it, and there is nobody left listening anyway -- writing one would
   // just put a packet on a path whose peer has been gone for a full idle period.
   //
   const bool silent = last_error_.type == NGTCP2_CCERR_TYPE_IDLE_CLOSE ||
                       last_error_.type == NGTCP2_CCERR_TYPE_DROP_CONN;

   if (conn_ && !silent && !ngtcp2_conn_in_closing_period(conn_) &&
       !ngtcp2_conn_in_draining_period(conn_))
   {
      std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> closebuf;
      ngtcp2_path_storage ps;
      if (auto packet = write_connection_close(closebuf, ps); !packet.empty())
         send_datagrams(ps.path, packet, packet.size());
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

int Http3ClientSession::handle_error(int /*rv*/)
{
   close();
   return -1;
}

// -------------------------------------------------------------------------------------------------

std::shared_ptr<http3::Http3Stream> Http3ClientSession::make_stream(int64_t id)
{
   return std::make_shared<Http3ClientStream>(*this, id);
}

//
// The connected socket has exactly one peer, so the path is of no interest here. What
// ngtcp2_conn_write_aggregate_pkt2() produced may be several QUIC packets, all but the last
// exactly `gso_size` bytes long -- without UDP_SEGMENT (which the server uses on its shared,
// unconnected socket) they go out one send() at a time.
//
int Http3ClientSession::send_datagrams(const ngtcp2_path& /*path*/, std::span<const uint8_t> data,
                                       size_t gso_size)
{
   while (!data.empty())
   {
      auto len = std::min(gso_size, data.size());
      boost::system::error_code ec;
      socket_.send(asio::buffer(data.data(), len), 0, ec);
      if (ec && ec != asio::error::would_block && ec != asio::error::try_again)
      {
         logw("[{}] send: {}", log_prefix_, ec.message());
         return 0; // best-effort; ngtcp2 will retransmit
      }
      data = data.subspan(len);
   }
   return 0;
}

int Http3ClientSession::on_read(std::span<const uint8_t> data)
{
   ngtcp2_pkt_info pi{};
   auto* path = ngtcp2_conn_get_path(conn_);
   if (http3::Http3Session::on_read(*path, pi, data) != 0)
      return -1;

   //
   // Unlike the server, which reads a whole batch of datagrams before answering it in one pass,
   // there is only ever one packet in flight here -- flush right away.
   //
   return flush_write();
}

// -------------------------------------------------------------------------------------------------

void Http3ClientSession::async_submit(SubmitHandler&& handler, boost::urls::url url,
                                      const Fields& headers)
{
   if (closed() || !h3())
   {
      loge("[{}] async_submit: session not ready", log_prefix_);
      std::move(handler)(errc::make_error_code(errc::operation_canceled), client::Request{nullptr});
      return;
   }

   int64_t stream_id = -1;
   if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr); rv != 0)
   {
      loge("[{}] async_submit: ngtcp2_conn_open_bidi_stream: {}", log_prefix_, ngtcp2_strerror(rv));
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   auto* stream = static_cast<Http3ClientStream*>(create_stream(stream_id));
   if (!stream->submit_request(url, headers))
   {
      erase_stream(stream_id);
      std::move(handler)(errc::make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   logd("[{}] async_submit: new stream ID: {}", stream->log_prefix, stream_id);
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

   co_spawn(executor, impl->do_session(Buffer{}), [impl](const std::exception_ptr& ex) mutable
   {
      if (ex)
         logw("client run: {}", what(ex));
      else
         logi("client run: done");
      impl.reset();
   });

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
