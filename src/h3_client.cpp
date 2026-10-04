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
#include "anyhttp/net.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <boost/url/url.hpp>

#include <boost/container/small_vector.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using namespace boost::asio;

using anyhttp::http3::format_hex;
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
// The client-role SSL_CTX for one outgoing QUIC connection. It is built per connection
// because the trust store comes from the Config; SSL_new() takes a reference, so the context can
// go once the session's SSL exists.
//
struct TlsClientContext
{
   explicit TlsClientContext(const Config& config)
   {
      ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new");

      http3::Http3Session::configure_tls_context(ctx, false);

      static constexpr unsigned char alpn[] = "\x02h3";
      SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn) - 1);

      if (config.tls_ca_file.empty())
      {
         if (SSL_CTX_set_default_verify_paths(ctx) != 1)
            throw std::runtime_error(std::format("SSL_CTX_set_default_verify_paths: {}",
                                                 ERR_error_string(ERR_get_error(), nullptr)));
      }
      else if (SSL_CTX_load_verify_locations(ctx, config.tls_ca_file.c_str(), nullptr) != 1)
         throw std::runtime_error(std::format("SSL_CTX_load_verify_locations: {}: {}",
                                              config.tls_ca_file,
                                              ERR_error_string(ERR_get_error(), nullptr)));

      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
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

} // namespace

// =================================================================================================
// Http3ClientStream / Http3ClientSession: the client's end of the shared HTTP/3 implementation.
// =================================================================================================

class Http3ClientSession;

namespace
{

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
   void on_failed(error_code ec) override;

   /// The request headers went out with the stream itself, see Http3ClientSession::async_submit().
   void submit_response(unsigned int, const Fields&) override {}

   /// Assembles and submits the request headers. Called once, right after the stream is created.
   bool submit_request(std::string_view method, const boost::urls::url& url, const Fields& headers);

   void async_get_response(client::Request::GetResponseHandler&& handler);
   void deliver_response();
   void deliver_failure();

   bool response_delivered = false;
   client::Request::GetResponseHandler response_handler;

   //
   // Why the stream died, remembered because async_get_response() may well be called only
   // afterwards -- a response that can never arrive must not leave its caller waiting forever.
   //
   error_code failure_ec;
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
         std::move(handler)(make_error_code(errc::connection_aborted), client::Response{nullptr});
         return;
      }
      static_cast<Http3ClientStream*>(stream)->async_get_response(std::move(handler));
   }
};

} // namespace

// -------------------------------------------------------------------------------------------------

class Http3ClientSession : public http3::Http3Session
{
public:
   Http3ClientSession(Executor executor, const Config& config);
   ~Http3ClientSession() override;

   //
   // Session::Impl
   //
   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;
   Task<void> do_session(Buffer&& data) override;
   void destroy() noexcept override;

   //
   // Connect-time setup. Returns 0 on success.
   //
   int init(asio::ip::udp::endpoint remote, const Config& config);

   //
   // Awaited by client::Client::Impl::async_connect() before handing the Session back to the
   // caller. Fires once the QUIC handshake has progressed far enough to create the HTTP/3 layer
   // (see setup_http3()), or once the connection has failed/closed before getting that far -- in
   // which case `ready()` is still false and the caller should synthesize an error.
   //
   auto wait_ready() { return ready_.wait(); }
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
   UdpSocket socket_;
   Event ready_; // see wait_ready()
};

// =================================================================================================
// Http3ClientStream implementation
// =================================================================================================

namespace
{

//
// The reading half of a client response: what http3::Http3Reader has for both roles, plus the
// status code, which only this role has. Its counterpart on the server is Http3RequestReader.
//
class Http3ResponseReader final : public http3::Http3Reader<client::Response::Impl>
{
public:
   using Http3Reader<client::Response::Impl>::Http3Reader;

   unsigned int status_code() const noexcept override
   {
      return stream ? stream->status_code : detached_status_code;
   }

   void detach() override
   {
      assert(stream);
      detached_status_code = stream->status_code;
      Http3Reader::detach();
   }

private:
   unsigned int detached_status_code = 0;
};

} // namespace

// -------------------------------------------------------------------------------------------------

Http3ClientStream::Http3ClientStream(Http3ClientSession& s, int64_t stream_id)
   : http3::Http3Stream(s, stream_id, http3::WriteMode::Staged)
{
}

Http3ClientStream::~Http3ClientStream()
{
   if (!response_delivered && response_handler)
      swap_and_invoke(response_handler, make_error_code(errc::connection_reset),
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
   //
   // A response with too large a header section is of no use: fail it, and stop the server from
   // sending its body.
   //
   if (header_limit_exceeded)
   {
      headers_received = false; // there is no response to deliver, see deliver_failure()
      failure_ec = errors::header_limit;
      session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
      deliver_failure();
      return;
   }

   using namespace boost::beast::http;
   mlogd("{} {}", status_code, obsolete_reason(int_to_status(status_code)));
   log_headers(log_prefix_, std::exchange(received_headers, {}));
   deliver_response();
}

void Http3ClientStream::on_failed(error_code ec)
{
   //
   // A stream closing gracefully (ec success, e.g. NGHTTP3_H3_NO_ERROR) still means no response
   // ever arrived if headers were never received -- never report success with a null Response.
   //
   if (!failure_ec) // the first reason is the one to report, see on_headers_complete()
      failure_ec =
         ec ? ec : error_code(boost::system::error_code(boost::beast::http::error::end_of_stream));
   deliver_failure();
}

void Http3ClientStream::deliver_failure()
{
   if (headers_received || response_delivered || !response_handler)
      return;

   response_delivered = true;
   swap_and_invoke(response_handler, failure_ec, client::Response{nullptr});
}

bool Http3ClientStream::submit_request(std::string_view method, const boost::urls::url& request_url,
                                       const Fields& headers)
{
   url = request_url;

   //
   // TODO: CONNECT
   //
   std::string method_str(method);
   std::string scheme(request_url.scheme());
   std::string target(request_url.encoded_target());
   std::string authority(request_url.encoded_host_and_port());

   auto nva = boost::container::small_vector<nghttp3_nv, 16>();
   nva.reserve(4 + std::distance(headers.begin(), headers.end()));
   nva.push_back(make_nv(":method", method_str));
   nva.push_back(make_nv(":scheme", scheme));
   nva.push_back(make_nv(":path", target));
   nva.push_back(make_nv(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         mlogw("async_submit: invalid header '{}': setting pseudo headers is not allowed",
               item.name_string());

      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   mlogd("{} {}", method_str, request_url.buffer());
   return submit_headers(nva, true /* request */);
}

void Http3ClientStream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   if (response_delivered)
   {
      complete_immediately(std::move(handler), get_executor(), errors::already_started,
                           client::Response{nullptr});
      return;
   }

   on_cancel(handler, [this] {
      mlogd("async_get_response: cancelled");
      if (response_handler)
         complete_later(std::move(response_handler), get_executor(), errors::canceled,
                        client::Response{nullptr});
   });

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
   auto response = client::Response{std::make_unique<Http3ResponseReader>(*this)};
   swap_and_invoke(response_handler, error_code{}, std::move(response));
}

// =================================================================================================
// Http3ClientSession implementation
// =================================================================================================

Http3ClientSession::Http3ClientSession(Executor executor, const Config& config)
   : http3::Http3Session(std::move(executor)), socket_(io::make_udp_socket(get_executor()))
{
   max_header_size_ = config.max_header_size;
}

Http3ClientSession::~Http3ClientSession()
{
   //
   // Tear the streams down while this object is still whole: destroying a stream fires pending
   // handlers, which reach back into the session.
   //
   clear_streams();
   mlogd("session deleted");
}

// -------------------------------------------------------------------------------------------------

int Http3ClientSession::init(asio::ip::udp::endpoint remote, const Config& config)
{
   TlsClientContext tls{config}; // may throw

   if (auto ec = io::open(socket_, remote))
   {
      loge("Http3ClientSession::init: open: {}", ec.message());
      return -1;
   }
   if (auto ec = io::connect(socket_, remote))
   {
      loge("Http3ClientSession::init: connect: {}", ec.message());
      return -1;
   }
   auto local = io::local_endpoint(socket_); // may throw

   log_prefix_ = http3::log_prefix(Role::client, "h3", remote.data(), remote.size());
   mlogd("session created");

   scid_.datalen = 17;
   if (RAND_bytes(scid_.data, static_cast<int>(scid_.datalen)) != 1)
   {
      mloge("init: RAND_bytes for SCID failed");
      return -1;
   }
   ngtcp2_cid dcid{};
   dcid.datalen = http3::QUIC_SCIDLEN;
   if (RAND_bytes(dcid.data, static_cast<int>(dcid.datalen)) != 1)
   {
      mloge("init: RAND_bytes for DCID failed");
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

   if (auto rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid_, &path, NGTCP2_PROTO_VER_V1,
                                        &callbacks, &settings, &params, nullptr, this);
       rv != 0)
   {
      mloge("ngtcp2_conn_client_new: {}", ngtcp2_strerror(rv));
      return -1;
   }

   if (setup_tls(tls.ctx, false /* client */) != 0)
      return -1;

   //
   // The certificate has to be valid for the origin, the host of the URL -- also when `remote` is
   // an alternative service it advertised (RFC 7838, section 2.1). An IP address is matched
   // against the certificate's IP SANs and not sent as SNI, which is for host names only.
   //
   const std::string host = config.url.host_address();
   auto* param = SSL_get0_param(ssl_);
   boost::system::error_code not_an_ip;
   asio::ip::make_address(host, not_an_ip);
   if (!not_an_ip ? X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1
                  : X509_VERIFY_PARAM_set1_host(param, host.data(), host.size()) != 1 ||
                       SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1)
   {
      mloge("init: can't verify certificates for '{}'", host);
      return -1;
   }

   mlogd("connecting, scid={}", format_hex(scid_.data, scid_.datalen));
   return 0;
}

// -------------------------------------------------------------------------------------------------

Task<void> Http3ClientSession::do_session(Buffer&&)
{
   if (flush_write() != 0)
   {
      signal_ready();
      co_return;
   }

   std::array<uint8_t, 64_k> buf;
   for (;;)
   {
      auto [ec, n] = co_await io::receive(socket_, asio::buffer(buf));
      if (ec)
      {
         if (ec != errc::operation_canceled)
            mlogw("receive: {}", ec.message());
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
   // async_get_response()/async_read_some() hanging forever.
   //
   fail_streams(make_error_code(errc::connection_reset));

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

   io::cancel(socket_);
   timer_.cancel();
   signal_ready();
}

void Http3ClientSession::signal_ready() { ready_.set(); }

int Http3ClientSession::handle_error(int /*rv*/)
{
   // X509_V_ERR_INVALID_CALL: the handshake never got as far as verifying anything
   if (ssl_)
      if (auto result = SSL_get_verify_result(ssl_);
          result != X509_V_OK && result != X509_V_ERR_INVALID_CALL)
         mlogw("server certificate: {}", X509_verify_cert_error_string(result));

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
      auto ec = io::send(socket_, asio::buffer(data.data(), len));
      if (ec && ec != errc::operation_would_block && ec != errc::resource_unavailable_try_again)
      {
         mlogw("send: {}", ec.message());
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

void Http3ClientSession::async_submit(SubmitHandler&& handler, std::string_view method,
                                      boost::urls::url url, const Fields& headers)
{
   if (closed() || !h3())
   {
      mloge("async_submit: session not ready");
      std::move(handler)(errors::canceled, client::Request{nullptr});
      return;
   }

   int64_t stream_id = -1;
   if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr); rv != 0)
   {
      mloge("async_submit: ngtcp2_conn_open_bidi_stream: {}", ngtcp2_strerror(rv));
      std::move(handler)(make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   auto* stream = static_cast<Http3ClientStream*>(create_stream(stream_id));
   if (!stream->submit_request(method, url, headers))
   {
      erase_stream(stream_id);
      std::move(handler)(make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   logd("[{}] async_submit: new stream ID: {}", stream->log_prefix_, stream_id);
   wake_write();

   complete_later(std::move(handler), get_executor(), error_code{},
                  client::Request{std::make_unique<Http3ClientWriter>(*stream)});
}

// =================================================================================================
// Entry point used by Client::Impl::async_connect() for Protocol::h3.
// =================================================================================================

Task<std::shared_ptr<Session::Impl>> async_connect_http3(Executor executor, std::string host,
                                                         std::string port, const Config& config)
{
   auto [resolved, endpoints] = co_await io::resolve(executor, host, port);
   if (resolved)
      throw_error(resolved);
   if (endpoints.empty())
      throw_error(make_error_code(errc::host_unreachable));
   const asio::ip::udp::endpoint remote{endpoints.front().address(), endpoints.front().port()};

   auto session = std::make_shared<Http3ClientSession>(executor, config);
   if (session->init(remote, config) != 0)
      throw_error(make_error_code(errc::connection_refused));

   std::shared_ptr<Session::Impl> impl = session;

   launch(executor, impl->do_session(Buffer{}),
          [impl, prefix = session->log_prefix()](const std::exception_ptr& ex) mutable {
             if (ex)
                logw("[{}] client run: {}", prefix, what(ex));
             else
                logi("[{}] client run: done", prefix);
             impl.reset();
          });

   //
   // The wait ends when the handshake has gotten far enough, when the connection has failed or
   // closed before that, or when the caller cancels. Only ready() tells these apart.
   //
   auto [ec] = co_await session->wait_ready();
   if (!session->ready())
      throw_error(make_error_code(errc::connection_refused));

   co_return session;
}

// =================================================================================================

} // namespace anyhttp::client
