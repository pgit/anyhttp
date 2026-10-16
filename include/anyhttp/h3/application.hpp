#pragma once

#include "anyhttp/quic/application.hpp"

#include <nghttp3/nghttp3.h>

#include <cstdint>
#include <memory>
#include <span>

namespace anyhttp::http3
{

class Http3Stream;

// =================================================================================================

//
// HTTP/3 on a QUIC connection: the nghttp3 connection sitting between ngtcp2 and the streams.
// Everything ngtcp2 reports about a stream goes through nghttp3 first, which strips the HTTP/3
// framing and calls back below with headers and body, and nghttp3 is what picks the stream to
// write next. The same class on both sides; the roles add how a stream comes into being
// (Http3ServerStream for a request that arrives, Http3ClientStream for one async_submit() opens).
//
class Http3Application : public quic::QuicApplication
{
public:
   Http3Application(quic::QuicSession& session, size_t max_header_size);
   ~Http3Application() override;

   nghttp3_conn* h3() const noexcept { return h3_; }

   /// The largest header section accepted from the peer, see Config::max_header_size.
   size_t max_header_size() const noexcept { return max_header_size_; }

   /// The stream with this ID, see QuicSession::find_stream().
   std::shared_ptr<Http3Stream> find_stream(int64_t id);

   //
   // QuicApplication
   //
   void fill_transport_params(ngtcp2_transport_params& params) override;
   int on_ready() override;
   int recv_stream_data(int64_t id, std::span<const uint8_t> data, bool fin) override;
   int acked_stream_data(int64_t id, uint64_t offset, uint64_t len) override;
   int stream_close(int64_t id, uint32_t flags, uint64_t app_error_code) override;
   int stream_reset(int64_t id, uint64_t app_error_code) override;
   int stop_sending(int64_t id, uint64_t app_error_code) override;
   int extend_max_stream_data(int64_t id) override;
   ngtcp2_ssize next_stream_data(int64_t& id, bool& fin, std::span<ngtcp2_vec> vec) override;
   int on_stream_data_written(int64_t id, size_t n) override;
   void on_stream_blocked(int64_t id) override;
   void on_stream_shut_wr(int64_t id) override;

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

   const std::string& log_prefix() const noexcept;

private:
   nghttp3_conn* h3_ = nullptr;
   size_t max_header_size_;
};

// =================================================================================================

} // namespace anyhttp::http3
