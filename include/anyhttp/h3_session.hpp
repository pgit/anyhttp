#pragma once

#include "anyhttp/session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace anyhttp::http3
{

class Http3Stream;

// =================================================================================================

//
// One QUIC connection carrying HTTP/3, as one anyhttp Session::Impl -- the same class on both
// sides. A QUIC connection is symmetric: past ngtcp2_conn_server_new()/ngtcp2_conn_client_new()
// and the handshake, both ends drive the very same ngtcp2/nghttp3 pair the same way, so the read
// loop, the write loop, the timers, the flow control and every callback bridge below are shared.
//
// What the roles still own themselves is how datagrams reach the connection (the server
// de-multiplexes many connections over one shared socket by connection ID, the client owns a
// connect()ed socket with exactly one peer), how a dead connection is torn down, and how streams
// come into being (accepted from the peer vs. opened by async_submit()). Those are the virtuals
// at the bottom.
//
class Http3Session : public Session::Impl
{
public:
   explicit Http3Session(asio::any_io_executor executor);
   ~Http3Session() override;

   //
   // Session::Impl
   //
   asio::any_io_executor get_executor() const noexcept override { return executor_; }

   ngtcp2_conn* conn() const noexcept { return conn_; }
   nghttp3_conn* h3() const noexcept { return h3_; }
   bool closed() const noexcept { return closed_; }
   const std::string& logPrefix() const noexcept { return log_prefix_; }

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
   // Grants the peer more *stream*-level send credit for `n` bytes of body just delivered to the
   // application. Deliberately NOT called as data arrives (see h3_cb_recv_data) -- only once
   // call_read_handler() actually hands bytes to the app, so a slow/absent reader keeps the
   // peer's flow control window for *this stream* genuinely constrained instead of nghttp3
   // buffering an unbounded backlog in pending_read. Connection-level credit is granted eagerly
   // regardless (see h3_cb_recv_data) since it's a pool shared with control/QPACK streams
   // nghttp3 manages on its own.
   //
   void consume_stream(int64_t stream_id, size_t n);

   //
   // Abort both directions of the stream (RESET_STREAM + STOP_SENDING), the QUIC equivalent of
   // HTTP/2's RST_STREAM. nghttp3 learns of the dead write side through the existing
   // NGTCP2_ERR_STREAM_SHUT_WR handling in write_pkt().
   //
   void reset_stream(int64_t stream_id, uint64_t app_error_code);

   //
   // Half-close just our read direction (STOP_SENDING), telling the peer to stop sending the
   // body while whatever we are still writing keeps flowing. Fires the local stream_stop_sending
   // callback, which is what tells nghttp3 about it.
   //
   void stop_reading(int64_t stream_id, uint64_t app_error_code);

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
   static int cb_extend_max_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user);
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

   /// Feeds one received datagram to ngtcp2. Marks the connection for writing, but does not write.
   int on_read(const ngtcp2_path& path, const ngtcp2_pkt_info& pi, std::span<const uint8_t> data);

   /// Creates the HTTP/3 layer (control + QPACK streams) on top of the QUIC connection.
   int setup_http3();

   /// Writes a CONNECTION_CLOSE frame into `buf`, returning what of it to send (may be empty).
   std::span<const uint8_t> write_connection_close(std::span<uint8_t> buf, ngtcp2_path_storage& ps);

   void arm_timer_from_ngtcp2();

   //
   // Tears down all streams. Called by both roles while their own state is still alive, because
   // destroying a stream fires pending handlers, which may reach back into the session.
   //
   void clear_streams();

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
   /// Creates the role's stream type (Http3ServerStream / Http3ClientStream).
   virtual std::shared_ptr<Http3Stream> make_stream(int64_t id) = 0;
   /// The HTTP/3 layer is up and requests can flow.
   virtual void on_http3_ready() {}
   /// A connection ID was minted for / retired from this connection (the server's demux table).
   virtual void on_new_cid(const ngtcp2_cid& cid) { (void)cid; }
   virtual void on_remove_cid(const ngtcp2_cid& cid) { (void)cid; }

   static ngtcp2_ssize write_pkt_cb(ngtcp2_conn*, ngtcp2_path* path, ngtcp2_pkt_info* pi,
                                    uint8_t* dest, size_t destlen, ngtcp2_tstamp ts,
                                    void* user_data);

protected:
   asio::any_io_executor executor_;

   ngtcp2_conn* conn_ = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr;
   ngtcp2_crypto_conn_ref conn_ref_{};

   nghttp3_conn* h3_ = nullptr;

   asio::steady_timer timer_; // ngtcp2 expiry (handshake / idle / PTO)
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

   std::unordered_map<int64_t, std::shared_ptr<Http3Stream>> streams_;
};

// =================================================================================================

} // namespace anyhttp::http3
