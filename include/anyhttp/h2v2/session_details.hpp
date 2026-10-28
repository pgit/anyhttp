#pragma once

//
// Definitions of the HTTP/2 session templates, nghttp2 version 2, instantiated only by
// src/h2v2/session.cpp -- the factories in anyhttp/h2/backend.hpp are what the generic server and
// client use instead. The counterpart of h2/session_details.hpp, with the same definitions in the
// same order, so that the two can be read side by side -- see H2_LIBRARY.
//

#include "anyhttp/h2v2/common.hpp"
#include "anyhttp/h2v2/session.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>

#include <nghttp2v2/nghttp2.h>

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Stream>
void NGHttp2SessionImpl<Stream>::destroy() noexcept
{
   auto ec = io::shutdown(stream_, io::Shutdown::both);
   // not_connected: the peer is gone already, which is what we wanted anyway
   logwd(ec && ec != errc::not_connected, //
         "[{}] destroy: socket shutdown: {}", log_prefix_, ec.message());
}

// =================================================================================================

#define mylogd(...)
// #define mylogd(...) mlogd(__VA_ARGS__) // too noisy, even for debug

//
// The send loop asks nghttp2_conn_write() to serialize into a buffer of ours and puts that on the
// wire, until the library has nothing more to send. Then it waits to be started again.
//
// This is where version 2 differs most from version 1. There, nghttp2_session_mem_send() handed
// out a pointer into the library's own buffer, which had to be copied or written before the next
// call, and whether the session was over had to be pieced together from want_read() and
// want_write(). Here the library fills a buffer we own -- one TLS record worth at a time, which is
// what the documentation asks for -- and says plainly when the connection is finished, by
// returning NGHTTP2_ERR_CLOSING.
//
template <typename Stream>
Task<void> NGHttp2SessionImpl<Stream>::send_loop()
{
   std::vector<uint8_t> buffer(16_k);

   for (;;)
   {
      //
      // Cleared before asking nghttp2, not before waiting: a start_write() while the write below
      // is suspended must still find its way to the wait.
      //
      send_ready_.clear();

      mylogd("send loop: nghttp2_conn_write...");
      const auto nwrite = nghttp2_conn_write(conn, buffer.data(), buffer.size(), timestamp());
      mylogd("send loop: nghttp2_conn_write... {} bytes", nwrite);

      //
      // The connection is over: a GOAWAY has gone one way or the other and every stream is done,
      // or nghttp2_conn_terminate() was called. Either way nothing more may be asked of conn.
      //
      if (nwrite == NGHTTP2_ERR_CLOSING)
      {
         mylogd("send loop: connection closing");
         closing_ = true;
         break;
      }

      if (nwrite < 0)
      {
         loge("[{}] send loop: nghttp2_conn_write: {}", log_prefix_,
              nghttp2_strerror(static_cast<int>(nwrite)));
         closing_ = true;
         io::close(stream_); // will also cancel the read loop
         throw std::runtime_error("nghttp2_conn_write");
      }

      if (nwrite > 0)
      {
         const auto bytes_to_write = static_cast<size_t>(nwrite);
         mylogd("send loop: writing {} bytes...", bytes_to_write);
         auto [ec, written] = co_await io::write(stream_, asio::buffer(buffer.data(), nwrite));
         if (ec)
         {
            // a peer that hung up is not our error
            if (ec == errc::broken_pipe || ec == errc::connection_reset)
               mlogi("send loop: error writing {} bytes: {}", bytes_to_write, ec.message());
            else
               mloge("send loop: error writing {} bytes: {}", bytes_to_write, ec.message());
            break;
         }
         mylogd("send loop: writing {} bytes... done, wrote {}", bytes_to_write, written);
         assert(bytes_to_write == written);
         continue; // there may well be more
      }

      //
      // Nothing to send. If nothing more is going to come in either, the session is done; the
      // GOAWAY the receive loop asks for has been drained by the call above.
      //
      if (recv_done_)
         break;

      //
      // nghttp2 may still want to be called back on a timer -- the SETTINGS acknowledgement
      // timeout, the rate limiter -- so arm it before going to sleep.
      //
      arm_timer_from_nghttp2();

      mylogd("send loop: waiting...");
      if (auto [ec] = co_await send_ready_.wait(); ec)
         break;
      mylogd("send loop: waiting... done");
   }

   timer_.cancel();

   //
   // Nothing more goes out, so whatever the receive loop might still read would go unused, and the
   // peer may well be waiting for us to close the connection, having received our GOAWAY (which,
   // for an error, RFC 9113 section 5.4.1 requires). So end the read as well, which ends the
   // session. See h2/session_details.hpp, where the same reasoning is spelled out at length.
   //
   io::cancel(stream_);

   //
   // Close whatever streams are left before letting go of them. Version 1 was called back for each
   // of them, nghttp2_session_terminate_session() closing every open stream on its way out, so
   // close_stream() did this; version 2 does not call back for the streams a connection that is
   // over leaves behind. Without this they would only be destroyed, and a pending read, write or
   // response would hear "cancelled" from the destructor -- where what has actually happened is
   // that the connection is gone.
   //
   mylogd("send loop: closing streams...");
   for (auto it = streams_.begin(); it != streams_.end();)
   {
      const auto id = it->first;
      close_stream(id);
      // close_stream() keeps a stream that a reader or an undelivered response still needs
      it = streams_.upper_bound(id);
   }
   streams_.clear();
   mylogd("send loop: closing streams... done");

   mylogd("send loop: done");
}

// -------------------------------------------------------------------------------------------------

//
// The read loop is better suited for implementation as a coroutine than the write loop,
// because it does not need to wait on re-activation by the user.
//
template <typename Stream>
Task<void> NGHttp2SessionImpl<Stream>::recv_loop()
{
   buffer_.reserve(64_k);

   while (!closing_)
   {
      auto free = buffer_.capacity() - buffer_.size();
      auto [ec, n] = co_await io::read_some(stream_, buffer_.prepare(free));
      if (ec)
      {
         mylogd("read: {}, ending session", ec.message());
         break;
      }
      buffer_.commit(n);

      handle_buffer_contents();
      start_write();
   }

   //
   // Nothing more comes in: either nghttp2 is done -- a GOAWAY has gone one way or the other, and
   // no stream is left -- or the read failed, mostly because the peer has closed the connection.
   // "Endpoints SHOULD always send a GOAWAY frame before closing a connection" (RFC 9113, section
   // 6.8), so send one, unless we have already. Not one with an error code: if the peer ended the
   // connection, it did nothing wrong. It is followed by the TLS "close_notify" and the FIN, see
   // do_session().
   //
   // A peer that waits for our end of the connection after its own sees that as a clean end. Many
   // do not (h2load, curl and nghttp among them): they close their socket right after their
   // GOAWAY, and answer whatever still arrives, this GOAWAY or the "close_notify", with an RST.
   //
   if (!goaway_sent_ && !closing_)
   {
      nghttp2_conn_terminate(conn, NGHTTP2_NO_ERROR);
      goaway_sent_ = true;
   }
   recv_done_ = true;
   start_write();

   mlogi("recv loop: done, served {} requests", request_counter_);
}

// =================================================================================================

template <typename Stream>
ServerSession<Stream>::ServerSession(server::Server::Impl& parent, Executor executor,
                                     Stream&& stream)
   : ServerReference(parent), super(anyhttp::log_prefix(Role::server, is_tls(stream) ? "h2" : "h2c",
                                                        io::remote_endpoint(stream)),
                                    executor, std::move(stream))
{
   max_header_size_ = parent.config().max_header_size;
   alt_svc_ = parent.alt_svc();
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
Task<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   buffer_ = std::move(buffer);
   auto callbacks = super::setup_callbacks();

   //
   // Everything version 1 set through an option object and a SETTINGS submission is one struct
   // here, which nghttp2_conn_server_new() copies. Flow control credit is extended by hand either
   // way, but in version 2 that is the only mode -- there is no auto window update to turn off.
   //
   nghttp2_settings settings;
   super::setup_settings(settings);
   settings.enable_connect_protocol = 1;

   if (auto rv = nghttp2_conn_server_new(&conn, &callbacks, &settings, nullptr, this))
   {
      mloge("nghttp2_conn_server_new: {}", nghttp2_strerror(rv));
      throw std::runtime_error("nghttp2_conn_server_new");
   }

   //
   // Continuing an HTTP/1.1 request upgraded to h2c has no counterpart in version 2: it dropped
   // nghttp2_session_upgrade2() along with the h2c upgrade itself, which RFC 9113 removed. An
   // upgrade is therefore refused before it gets this far, see server_impl.cpp.
   //
   assert(!upgrade_);

   //
   // Let nghttp2 parse what we have received so far.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await when_both(send_loop(), recv_loop());

   mlogd("server session done");

   //
   // End the stream itself: over TLS, that is the "close_notify" the peer needs to tell the end
   // of the data from a connection that was cut, see async_teardown().
   //
   if (auto ec = co_await async_teardown(stream_); ec)
      mlogd("teardown: {}", ec.message());

   //
   // Then send a FIN, and let go of the socket only once the peer has ended its side as well,
   // reading and dropping what still comes in for two seconds at most: closing it right away would
   // answer a peer that sent anything after our GOAWAY with an RST, which may cost it that GOAWAY.
   // See io::drain().
   //
   if (auto ec = io::shutdown(stream_, io::Shutdown::send); !ec)
   {
      if (auto drained = co_await io::drain(stream_, this->get_executor(), std::chrono::seconds(2)))
         mlogd("dropped {} bytes the peer sent after the session ended", drained);
   }
   else if (ec != errc::not_connected) // the peer may be gone already
      mlogw("shutdown: {}", ec.message());
   io::close(stream_);

   nghttp2_conn_del(conn);
   conn = nullptr;
   mlogd("server session deleted");
}

// =================================================================================================

template <typename Stream>
ClientSession<Stream>::ClientSession(client::Client::Impl& parent, Executor executor,
                                     Stream&& stream)
   : ClientReference(parent), super(anyhttp::log_prefix(Role::client, is_tls(stream) ? "h2" : "h2c",
                                                        io::remote_endpoint(stream)),
                                    executor, std::move(stream))
{
   max_header_size_ = parent.config().max_header_size;
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
Task<void> ClientSession<Stream>::do_session(Buffer&& buffer)
{
   buffer_ = std::move(buffer);
   auto callbacks = super::setup_callbacks();

   //
   // The same settings as the server's, minus the CONNECT protocol, which is a server's to offer.
   // Version 1 also had to enable the ALTSVC extension frame here, or nghttp2 would have dropped
   // it unseen; version 2 does not implement that frame at all, so an alternative service only
   // ever arrives as a response header field -- see on_end_headers().
   //
   nghttp2_settings settings;
   super::setup_settings(settings);

   if (auto rv = nghttp2_conn_client_new(&conn, &callbacks, &settings, nullptr, this))
   {
      mloge("nghttp2_conn_client_new: {}", nghttp2_strerror(rv));
      throw std::runtime_error("nghttp2_conn_client_new");
   }

   //
   // Let nghttp2 parse what we have received so far.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await when_both(send_loop(), recv_loop());

   mlogd("client session done");

   nghttp2_conn_del(conn);
   conn = nullptr;
   mlogd("client session deleted");
}

// =================================================================================================

} // namespace anyhttp::nghttp2
