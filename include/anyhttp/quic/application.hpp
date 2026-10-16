#pragma once

#include "anyhttp/common.hpp"
#include "anyhttp/session.hpp"

#include <ngtcp2/ngtcp2.h>

#include <boost/url/url.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace anyhttp::quic
{

class QuicSession;
class QuicStream;

// =================================================================================================

//
// What runs on top of a QUIC connection: HTTP/3 (http3::Http3Application, on nghttp3) or plain
// streams. A session owns exactly one, picked by the ALPN the handshake negotiates -- on the server
// in the TLS ALPN callback, on the client before it connects -- and forwards to it every ngtcp2
// callback that is about streams rather than about the connection. It is also the write
// scheduler: QuicSession::write_pkt() asks it which stream goes into the next packet.
//
// The integer results follow ngtcp2's callbacks: 0 for success, NGTCP2_ERR_CALLBACK_FAILURE to
// close the connection, after setting its error with QuicSession::set_application_error().
//
class QuicApplication
{
public:
   explicit QuicApplication(QuicSession& session) noexcept : session_(session) {}
   virtual ~QuicApplication() = default;

   QuicApplication(const QuicApplication&) = delete;
   QuicApplication& operator=(const QuicApplication&) = delete;

   QuicSession& session() const noexcept { return session_; }

   //
   // The stream limits and flow control windows this protocol wants. Called with the session's
   // defaults filled in, before they go out: on the client into the ClientHello, on the server
   // when the ALPN has been chosen, which is before its handshake keys are installed.
   //
   virtual void fill_transport_params(ngtcp2_transport_params& params) = 0;

   //
   // The handshake got far enough for streams to flow: open whatever the protocol needs before
   // the first one (HTTP/3: the control and QPACK streams).
   //
   virtual int on_ready() = 0;

   /// Creates the stream object for a stream the peer opened or that we are opening.
   virtual std::shared_ptr<QuicStream> make_stream(int64_t id) = 0;

   //
   // ngtcp2's stream callbacks, see ngtcp2_callbacks.
   //
   virtual int recv_stream_data(int64_t id, std::span<const uint8_t> data, bool fin) = 0;
   virtual int acked_stream_data(int64_t id, uint64_t offset, uint64_t len) = 0;
   virtual int stream_close(int64_t id, uint32_t flags, uint64_t app_error_code) = 0;
   virtual int stream_reset(int64_t id, uint64_t app_error_code) = 0;
   virtual int stop_sending(int64_t id, uint64_t app_error_code) = 0;
   virtual int extend_max_stream_data(int64_t id) = 0;

   //
   // Write scheduling, from QuicSession::write_pkt(), which is only called when the connection
   // has credit left.
   //
   // next_stream_data() picks the stream to write next and points `vec` at its data, returning
   // how many entries it used: 0 with `id` < 0 if there is nothing to write, a negative value to
   // fail the connection. ngtcp2 then reports back what became of it: how many of those bytes went
   // into a packet (on_stream_data_written()), or that the stream has to wait for credit
   // (on_stream_blocked()) or can't be written any more (on_stream_shut_wr()).
   //
   virtual ngtcp2_ssize next_stream_data(int64_t& id, bool& fin, std::span<ngtcp2_vec> vec) = 0;
   virtual int on_stream_data_written(int64_t id, size_t n) = 0;
   virtual void on_stream_blocked(int64_t id) = 0;
   virtual void on_stream_shut_wr(int64_t id) = 0;

   //
   // Session::Impl::async_submit(), which only HTTP/3's client side has a use for. Everyone else
   // completes it with errc::operation_not_supported.
   //
   virtual void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                             const Fields& headers);

protected:
   QuicSession& session_;
};

// =================================================================================================

} // namespace anyhttp::quic
