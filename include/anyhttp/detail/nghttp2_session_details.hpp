
#include "anyhttp/any_async_stream.hpp"
#include "anyhttp/nghttp2_session.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>

#include <nghttp2/nghttp2.h>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

using namespace boost::asio;
using namespace boost::beast;
using socket = asio::ip::tcp::socket;

inline auto& get_socket(socket& socket) { return socket; }
inline auto& get_socket(tcp_stream& stream) { return stream.socket(); }
inline auto& get_socket(ssl::stream<socket>& stream) { return stream.lowest_layer(); }
inline auto& get_socket(AnyAsyncStream& stream) { return stream.get_socket(); }

// =================================================================================================

template <typename Stream>
void NGHttp2SessionImpl<Stream>::destroy(std::shared_ptr<Session::Impl> self) noexcept
{
   // post(get_executor(), [this, self]() mutable {
   boost::system::error_code ec;
   std::ignore = get_socket(m_stream).shutdown(socket_base::shutdown_both, ec);
   logwi(ec, "[{}] destroy: socket shutdown: {}", m_logPrefix, ec.message());
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
awaitable<void> NGHttp2SessionImpl<Stream>::send_loop()
{
   Buffer buffer;
   buffer.reserve(1460);

   for (;;)
   {
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
         get_socket(m_stream).close(); // will also cancel the read loop
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
         const std::array<asio::const_buffer, 2> seq{buffer.data(), asio::buffer(data, nread)};
         mylogd("send loop: writing {} bytes...", bytes_to_write);
         auto [ec, written] = co_await asio::async_write(m_stream, seq, as_tuple(deferred));
         if (ec)
         {
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
         co_await async_wait_send(deferred);
         mylogd("send loop: waiting... done");
      }
   }

   mylogd("send loop: destroying streams...");
   m_streams.clear();
   mylogd("send loop: destroying streams... done");

   mylogd("send loop: done");
}

// -------------------------------------------------------------------------------------------------

//
// The read loop is better suited for implementation as a coroutine than the write loop,
// because it does not need to wait on re-activation by the user.
//
template <typename Stream>
awaitable<void> NGHttp2SessionImpl<Stream>::recv_loop()
{
   m_buffer.reserve(64 * 1024);

   unsigned int reason = NGHTTP2_NO_ERROR;
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto free = m_buffer.capacity() - m_buffer.size();
      auto [ec, n] = co_await m_stream.async_read_some(m_buffer.prepare(free), as_tuple(deferred));
      if (ec)
      {
         mylogd("read: {}, terminating session", ec.message());
         reason = NGHTTP2_STREAM_CLOSED;
         break;
      }
      m_buffer.commit(n);

      handle_buffer_contents();
      start_write();
   }

   nghttp2_session_terminate_session(session, reason);
   start_write();

   mlogi("recv loop: done, served {} requests", m_requestCounter);
}

// =================================================================================================

template <typename Stream>
ServerSession<Stream>::ServerSession(server::Server::Impl& parent, any_io_executor executor,
                                     Stream&& stream)
   : ServerReference(parent), super("\x1b[1;31mserver\x1b[0m", executor, std::move(stream))
{
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
awaitable<void> ServerSession<Stream>::do_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);
   // get_socket(m_stream).set_option(ip::tcp::no_delay(true));
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

   if (auto rv = nghttp2_session_server_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_server_new");

#if 0
   const uint32_t window_size = 1024 * 1024;
   std::array<nghttp2_settings_entry, 2> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size}}};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
#else
   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &ent, 1);
#endif

   //
   // Let NGHTTP2 parse what we have received so far.
   // This must happen after submitting the server settings.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await (send_loop() && recv_loop());

   mlogd("server session done");

   nghttp2_session_del(session);
   session = nullptr;
   mlogd("server session deleted");
}

// =================================================================================================

template <typename Stream>
ClientSession<Stream>::ClientSession(client::Client::Impl& parent, any_io_executor executor,
                                     Stream&& stream)
   : ClientReference(parent), super("\x1b[1;32mclient\x1b[0m", executor, std::move(stream))
{
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
awaitable<void> ClientSession<Stream>::do_session(Buffer&& buffer)
{
   m_buffer = std::move(buffer);
   get_socket(m_stream).set_option(ip::tcp::no_delay(true));
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

   if (auto rv = nghttp2_session_client_new2(&session, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_client_new");

#if 1
   // const uint32_t window_size = 256 * 1024 * 1024;
   // const uint32_t window_size = 64 * 1024;
   const uint32_t window_size = 1024 * 1024;
   std::array<nghttp2_settings_entry, 2> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                                             {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size}}};
   nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
   nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, window_size);
#endif

   //
   // Let NGHTTP2 parse what we have received so far.
   //
   handle_buffer_contents();

   //
   // send/receive loop
   //
   co_await (send_loop() && recv_loop());

   mlogd("client session done");

   nghttp2_session_del(session);
   session = nullptr;
   mlogd("client session deleted");
}

// =================================================================================================

} // namespace anyhttp::nghttp2
