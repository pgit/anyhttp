#pragma once

//
// Definitions of the HTTP/2 session templates, instantiated only by src/h2/session.cpp -- the
// factories in anyhttp/h2/backend.hpp are what the generic server and client use instead.
//

#include "anyhttp/h2/common.hpp"
#include "anyhttp/h2/session.hpp"
#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>

#include <nghttp2/nghttp2.h>

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Stream>
void NGHttp2SessionImpl<Stream>::destroy() noexcept
{
   // post(get_executor(), [this, self]() mutable {
   auto ec = io::shutdown(stream_, io::Shutdown::both);
   // not_connected: the peer is gone already, which is what we wanted anyway
   logwd(ec && ec != errc::not_connected, //
         "[{}] destroy: socket shutdown: {}", log_prefix_, ec.message());
   // });
}

// =================================================================================================

#define mylogd(...)
// #define mylogd(...) mlogd(__VA_ARGS__) // too noisy, even for debug

//
// Implementing the send loop as a coroutine does not make much sense, as it may run out
// of work and then needs to wait on a channel to be activated again. Doing this with
// a normal, callback-based completion handler is probably easier.
//
// This would also allow easier customization with third party stream objects by not requiring
// coroutines for compilation.
//
// This function calls nghttp2_session_mem_send() and collects the retrieved data in a send buffer,
// until either the buffer is full or no more data is returned. Then, the buffered data is written
// to the stream. Finally, if still no more data is returned, it waits for a signal to resume.
//
template <typename Stream>
Task<void> NGHttp2SessionImpl<Stream>::send_loop()
{
   Buffer buffer;
   buffer.reserve(1460);

   for (;;)
   {
      //
      // Cleared before asking nghttp2, not before waiting: a start_write() while the write below
      // is suspended must still find its way to the wait.
      //
      send_ready_.clear();

      //
      // Retrieve a chunk of data to be sent from NGHTTP2.
      // The buffer is valid until next call nghttp2_session_mem_send, so we don't need to copy it.
      // We still may want to copy it into a local buffer to bundle many small writes.
      //
      const uint8_t* data;

      mylogd("send loop: nghttp2_session_mem_send...");
      const auto nread = nghttp2_session_mem_send(session, &data);
      mylogd("send loop: nghttp2_session_mem_send... {} bytes", nread);
      if (nread < 0)
      {
         logw("send loop: closing stream and throwing");
         io::close(stream_); // will also cancel the read loop
         throw std::runtime_error("nghttp2_session_mem_send");
      }

      //
      // If the new chunk fits into the buffer, accumulate and send later.
      //
      if (nread && nread <= (buffer.capacity() - buffer.size()))
      {
         auto copied = asio::buffer_copy(buffer.prepare(nread), asio::buffer(data, nread));
         assert(nread == copied);
         buffer.commit(nread);
         mylogd("send loop: buffered {} more bytes, total {}", nread, buffer.data().size());
      }

      //
      // Is there anything to send in the buffer and/or the newly received chunk?
      // If yes, combine both into a buffer sequence and pass it to async_write().
      // Afterwards, go back up and ask NGHTTP2 if there is more data to send.
      //
      else if (const auto bytes_to_write = buffer.size() + nread; bytes_to_write > 0)
      {
         const auto seq =
            std::to_array<asio::const_buffer>({buffer.data(), asio::buffer(data, nread)});
         mylogd("send loop: writing {} bytes...", bytes_to_write);
         auto [ec, written] = co_await io::write(stream_, seq);
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
         buffer.clear(); // consume(written);
      }

      //
      // Finally, if there is really nothing to send any more, wait to be started again.
      //
      else if (nread == 0)
      {
         if (nghttp2_session_want_write(session) && nghttp2_session_want_read(session))
            mylogd("send loop: session still wants to read and write");
         else if (nghttp2_session_want_write(session))
            mylogd("send loop: session still wants to write");
         else if (nghttp2_session_want_read(session))
            mylogd("send loop: session still wants to read");
         else
            break; // nghttp2 doesn't want to send or receive any more, so we are done

         mylogd("send loop: waiting...");
         if (auto [ec] = co_await send_ready_.wait(); ec)
            break;
         mylogd("send loop: waiting... done");
      }
   }

   //
   // Nothing more goes out: nghttp2 wants neither to read nor to write any more, or the connection
   // broke. Whatever the receive loop might still read would go unused, and the peer may well be
   // waiting for us to close the connection, having received our GOAWAY (which, for an error,
   // RFC 9113 section 5.4.1 requires). So end the read as well, which ends the session.
   //
   // Before, this happened by accident of timing: start_write() drained the GOAWAY into the socket
   // before the receive loop checked nghttp2_session_want_read() again. Now that the send loop is
   // woken by a posted Event, the receive loop gets there first.
   //
   io::cancel(stream_);

   mylogd("send loop: destroying streams...");
   streams_.clear();
   mylogd("send loop: destroying streams... done");

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

   unsigned int reason = NGHTTP2_NO_ERROR;
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto free = buffer_.capacity() - buffer_.size();
      auto [ec, n] = co_await io::read_some(stream_, buffer_.prepare(free));
      if (ec)
      {
         mylogd("read: {}, terminating session", ec.message());
         reason = NGHTTP2_STREAM_CLOSED;
         break;
      }
      buffer_.commit(n);

      handle_buffer_contents();
      start_write();
   }

   nghttp2_session_terminate_session(session, reason);
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
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't
   //
   auto options = nghttp2_option_new();
   nghttp2_option_set_no_http_messaging(options.get(), 0); // h2spec: fails ~16 tests if 1
   nghttp2_option_set_no_auto_window_update(options.get(), 1);
   nghttp2_option_set_max_send_header_block_length(options.get(), 1_m);
   nghttp2_option_set_max_continuations(options.get(), max_continuations(max_header_size_));

   if (auto rv = nghttp2_session_server_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_server_new");

#if 1
   const uint32_t window_size = 1_m;
   //
   // No SETTINGS_MAX_HEADER_LIST_SIZE: it defaults to unlimited and is advisory anyway, as nghttp2
   // enforces it in neither direction. Header sections beyond max_header_size are rejected where
   // they arrive, see on_header_callback().
   //
   auto iv =
      std::to_array<nghttp2_settings_entry>({{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size},
                                             {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1}});
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
   nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, window_size);
#else
   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);
#endif

   //
   // Continue an HTTP/1.1 request upgraded to h2c as stream 1. It has been received completely,
   // so nghttp2 opens it half-closed (remote). No HEADERS frame will arrive for it, so do what
   // on_begin_headers_callback() and on_frame_recv_callback() would have done.
   //
   if (upgrade_)
   {
      const auto& settings = upgrade_->settings;
      const bool head_request = upgrade_->method == "HEAD";
      if (auto rv =
             nghttp2_session_upgrade2(session, reinterpret_cast<const uint8_t*>(settings.data()),
                                      settings.size(), head_request, nullptr))
      {
         mloge("nghttp2_session_upgrade2: {}", nghttp2_strerror(rv));
         nghttp2_session_terminate_session(session, NGHTTP2_PROTOCOL_ERROR);
      }
      else
      {
         auto stream = this->create_stream(1);
         stream->method = std::move(upgrade_->method);
         stream->url = std::move(upgrade_->url);
         stream->fields = std::move(upgrade_->fields);
         mlogd("upgraded from HTTP/1.1: {} {}", stream->method, stream->url.buffer());
         stream->on_request();
         stream->on_eof(session, 1);
      }
      upgrade_.reset();
   }

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
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

   nghttp2_session_del(session);
   session = nullptr;
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
   // disable automatic WINDOW update as we are sending window updates ourselves
   // https://github.com/nghttp2/nghttp2/issues/446
   //
   // pynghttp2 does also disable "HTTP messaging semantics", but we don't
   //
   auto options = nghttp2_option_new();
   nghttp2_option_set_no_http_messaging(options.get(), 1);
   nghttp2_option_set_no_auto_window_update(options.get(), 1);
   nghttp2_option_set_max_send_header_block_length(options.get(), 1_m);
   nghttp2_option_set_max_continuations(options.get(), max_continuations(max_header_size_));

   //
   // ALTSVC (RFC 7838, section 4) is an extension frame: without this, nghttp2 drops it before
   // on_frame_recv_callback() ever sees it. It is how a server may advertise its HTTP/3 endpoint
   // without waiting for a request, see Config::follow_alt_svc.
   //
   nghttp2_option_set_builtin_recv_extension_type(options.get(), NGHTTP2_ALTSVC);

   if (auto rv = nghttp2_session_client_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_client_new");

#if 1
   const uint32_t window_size = 1_m;
   //
   // No SETTINGS_MAX_HEADER_LIST_SIZE: it defaults to unlimited and is advisory anyway, as nghttp2
   // enforces it in neither direction. Header sections beyond max_header_size are rejected where
   // they arrive, see on_header_callback().
   //
   auto iv =
      std::to_array<nghttp2_settings_entry>({{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size}});
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
   nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, window_size);
#else
   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);
#endif

   //
   // Let NGHTTP2 parse what we have received so far.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await when_both(send_loop(), recv_loop());

   mlogd("client session done");

   nghttp2_session_del(session);
   session = nullptr;
   mlogd("client session deleted");
}

// =================================================================================================

} // namespace anyhttp::nghttp2
