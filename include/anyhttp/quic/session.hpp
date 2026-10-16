#pragma once

#include "anyhttp/quic/application.hpp"
#include "anyhttp/session_impl.hpp"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <openssl/ssl.h>

//
// ngtcp2 has one crypto backend for BoringSSL and AWS-LC, and another one for OpenSSL (3.5+).
// Which one we build against follows from the TLS headers (see TLS_LIBRARY in CMakeLists.txt).
//
#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
#define ANYHTTP_QUIC_BORINGSSL 1
#include <ngtcp2/ngtcp2_crypto_boringssl.h>
#else
#define ANYHTTP_QUIC_BORINGSSL 0
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#endif

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anyhttp::quic
{

class QuicStream;

// =================================================================================================

//
// One QUIC connection, as one anyhttp Session::Impl -- the same class on both sides. A QUIC
// connection is symmetric: past ngtcp2_conn_server_new()/ngtcp2_conn_client_new() and the
// handshake, both ends drive the very same ngtcp2 connection the same way, so the read loop, the
// write loop, the timers, the flow control and every callback bridge below are shared.
//
// What runs on top of the connection is the QuicApplication's business: the session forwards the
// stream callbacks to it, and lets it pick what to write next. Which one that is depends on the
// ALPN, and the ALPN is only settled during the handshake -- which is why it is composed rather
// than derived from.
//
// What the roles still own themselves is how datagrams reach the connection (the server
// de-multiplexes many connections over one shared socket by connection ID, the client owns a
// connect()ed socket with exactly one peer), how a dead connection is torn down and how a request
// is submitted. Those are the virtuals at the bottom.
//
class QuicSession : public Session::Impl
{
public:
   explicit QuicSession(const Executor& executor);
   ~QuicSession() override;

   //
   // Session::Impl
   //
   Executor get_executor() const noexcept override { return executor_; }

   ngtcp2_conn* conn() const noexcept { return conn_; }
   bool closed() const noexcept { return closed_; }
   const std::string& log_prefix() const noexcept { return log_prefix_; }
   const ngtcp2_cid& scid() const noexcept { return scid_; }

   /// The protocol running on top, once the ALPN is known (null before).
   QuicApplication* application() const noexcept { return application_.get(); }

   /// Whether the application is up and streams can flow, see QuicApplication::on_ready().
   bool application_ready() const noexcept { return application_ready_; }

   //
   // Returns a shared_ptr, not a raw pointer: callers routinely invoke user handlers on the
   // stream they looked up, and those can drop the last reference to it (the coroutine they
   // resume destroying its Request/Response), which erases the stream from streams_. Holding
   // an owning reference for the duration of the lookup keeps that from becoming a
   // use-after-free.
   //
   std::shared_ptr<QuicStream> find_stream(int64_t id);
   QuicStream* create_stream(int64_t id);
   void erase_stream(int64_t id);

   //
   // Grants the peer more *stream*-level send credit for `n` bytes just delivered to the
   // application. Deliberately not called as data arrives -- only once a stream actually hands
   // bytes to its reader, so a slow or absent reader keeps the peer's flow control window for
   // *that stream* genuinely constrained instead of an unbounded backlog building up in front of
   // it. Connection-level credit is granted as data arrives, see the applications.
   //
   void consume_stream(int64_t stream_id, size_t n);

   //
   // Abort both directions of the stream (RESET_STREAM + STOP_SENDING), the QUIC equivalent of
   // HTTP/2's RST_STREAM. The application learns of the dead write side through the
   // NGTCP2_ERR_STREAM_SHUT_WR handling in write_pkt().
   //
   void reset_stream(int64_t stream_id, uint64_t app_error_code);

   //
   // Half-close just our read direction (STOP_SENDING), telling the peer to stop sending while
   // whatever we are still writing keeps flowing. Fires the local stream_stop_sending callback,
   // which is what tells the application about it.
   //
   void stop_reading(int64_t stream_id, uint64_t app_error_code);

   /// The application error code the connection is closed with, see QuicApplication.
   void set_application_error(uint64_t app_error_code);

   //
   // Called whenever new data was queued outside of a packet arriving or a timer firing; makes
   // sure the write loop runs. One flush per wake, not one per submission: a response submits
   // its headers, its body and its EOF separately, and posting for each means the first pass
   // writes everything and the rest walk the connection for nothing.
   //
   void wake_write();

   //
   // Writes out whatever ngtcp2 has queued and re-arms the expiry timer. Reading does not write:
   // the server feeds a whole batch of datagrams to ngtcp2 and flushes once at the end, so a
   // response goes out as one big GSO batch instead of several small ones.
   //
   int flush_write();

   int write_streams();
   ngtcp2_ssize write_pkt(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                          ngtcp2_tstamp ts);
   void update_timer();
   int handle_expiry();

   //
   // Prepares a role's SSL_CTX for QUIC. With BoringSSL, that is where ngtcp2 hooks in; with
   // OpenSSL, it happens per SSL, in setup_tls().
   //
   static void configure_tls_context(SSL_CTX* ctx, bool is_server);

   //
   // ngtcp2 <-> ngtcp2 crypto backend bridge, reached through SSL_get_app_data().
   //
   static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* ref)
   {
      return static_cast<QuicSession*>(ref->user_data)->conn_;
   }

   /// The session an SSL of ours belongs to, e.g. in a TLS callback.
   static QuicSession* from_ssl(SSL* ssl);

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
   static int cb_extend_max_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user);
   static int cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                     void* user, void*);
   static int cb_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t final_size,
                              uint64_t app_error_code, void* user, void*);
   static int cb_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t max_data,
                                        void* user, void*);
   static int cb_recv_rx_key(ngtcp2_conn*, ngtcp2_encryption_level level, void* user);

protected:
   //
   // Connection setup, shared by both roles' init(). fill_callbacks() installs everything that
   // isn't role-specific; the role adds its own (recv_client_initial / client_initial + recv_retry)
   // before handing the table to ngtcp2_conn_server_new()/ngtcp2_conn_client_new().
   //
   void fill_callbacks(ngtcp2_callbacks& callbacks);
   void fill_settings(ngtcp2_settings& settings, ngtcp2_transport_params& params,
                      std::chrono::nanoseconds idle_timeout);
   int setup_tls(SSL_CTX* ssl_ctx, bool is_server);

   //
   // Installs the protocol running on top. The client does so before creating the connection and
   // passes the transport parameters it filled in to ngtcp2_conn_client_new(); the server once
   // the ALPN is chosen, and replaces its transport parameters with what the application wants.
   //
   void set_application(std::unique_ptr<QuicApplication> application);

   /// Feeds one received datagram to ngtcp2. Marks the connection for writing, but does not write.
   int on_read(const ngtcp2_path& path, const ngtcp2_pkt_info& pi, std::span<const uint8_t> data);

   /// Brings the application up, see QuicApplication::on_ready().
   int start_application();

   /// Writes a CONNECTION_CLOSE frame into `buf`, returning what of it to send (may be empty).
   std::span<const uint8_t> write_connection_close(std::span<uint8_t> buf, ngtcp2_path_storage& ps);

   void arm_timer_from_ngtcp2();

   //
   // Tears down all streams. Called by both roles while their own state is still alive, because
   // destroying a stream fires pending handlers, which may reach back into the session.
   //
   void clear_streams();

   //
   // The connection is over: fails every stream that has not completed yet, instead of leaving
   // its operations to succeed into the void or hang until the session object goes away.
   //
   void fail_streams(error_code ec);

   //
   // Role-specific: everything a QUIC connection cannot decide on its own.
   //
   /// The connection is dead or dying; the role decides how it goes away (closing period and a
   /// buffered CONNECTION_CLOSE on the server, plain teardown on the client).
   virtual int handle_error(int rv) = 0;
   /// Puts the packets ngtcp2 just produced on the wire. `data` holds one or more QUIC packets,
   /// all but the last exactly `gso_size` bytes long.
   virtual int send_datagrams(const ngtcp2_path& path, std::span<const uint8_t> data,
                              size_t gso_size) = 0;
   /// The application is up and streams can flow.
   virtual void on_application_ready() {}
   /// A connection ID was minted for / retired from this connection (the server's demux table).
   virtual void on_new_cid(const ngtcp2_cid& cid) { (void)cid; }
   virtual void on_remove_cid(const ngtcp2_cid& cid) { (void)cid; }

   static ngtcp2_ssize write_pkt_cb(ngtcp2_conn*, ngtcp2_path* path, ngtcp2_pkt_info* pi,
                                    uint8_t* dest, size_t destlen, ngtcp2_tstamp ts,
                                    void* user_data);

protected:
   Executor executor_;

   ngtcp2_conn* conn_ = nullptr;
   ngtcp2_cid scid_{}; // the connection ID this end chose for itself when creating conn_
#if ANYHTTP_QUIC_BORINGSSL
   SSL* ssl_ = nullptr; // also ngtcp2's TLS native handle
#else
   SSL* ssl_ = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr; // ngtcp2's TLS native handle, wrapping ssl_
#endif
   ngtcp2_crypto_conn_ref conn_ref_{};

   std::unique_ptr<QuicApplication> application_;
   bool application_ready_ = false;

   Timer timer_; // ngtcp2 expiry (handshake / idle / PTO)
   ngtcp2_ccerr last_error_{};
   bool closed_ = false;

   std::string log_prefix_;

   bool write_posted_ = false; // a wake_write() flush is already on the way

   //
   // Aggregated TX buffer: ngtcp2_conn_write_aggregate_pkt2() packs as many same-sized packets as
   // it can (control/QPACK streams, body data, ...) into this buffer so they can all be flushed
   // with a single sendmsg()+UDP_SEGMENT (GSO) call instead of one send per QUIC packet.
   //
   std::vector<uint8_t> tx_buf_;

   std::unordered_map<int64_t, std::shared_ptr<QuicStream>> streams_;
   size_t streams_created_ = 0; // for the log line a session ends with
};

// =================================================================================================

} // namespace anyhttp::quic
