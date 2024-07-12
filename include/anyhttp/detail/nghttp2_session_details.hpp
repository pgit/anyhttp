
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>
#include <boost/url/format.hpp>

#include <nghttp2/nghttp2.h>
#include <string>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::nghttp2
{

// =================================================================================================

using namespace asio;
using namespace boost::beast;
using socket = asio::ip::tcp::socket;

inline auto& get_socket(socket& socket) { return socket; }
inline auto& get_socket(tcp_stream& stream) { return stream.socket(); }
inline auto& get_socket(ssl::stream<socket>& stream) { return stream.lowest_layer(); }

template <typename Stream>
void NGHTttp2SessionImpl<Stream>::destroy()
{
   boost::system::error_code ec;
   std::ignore = get_socket(m_stream).shutdown(boost::asio::socket_base::shutdown_both, ec);
   logi("[{}] shutdown: {}", m_logPrefix, ec.message());
}

// =================================================================================================

#define mylogd(...)
// #define mylogd(...) mlogd(__VA_ARGS__) // too noisy, even for debug

//
// Implementing the send loop as a coroutine does not make much sense, as it may run out
// of work and then needs to wait on a channel to be activated again. Doing this with
// a normal, callback-based completion handler is probably easier.
//
// This would also allow easier customization with thirdparty stream objects by not requiring
// coroutines for compilation.
//
// This function calls nghttp2_session_mem_send() and collects the retrieved data in a send buffer,
// until either the buffer is full or no more data is returned. Then, the buffered data is written
// to the stream. Finally, if still no more data is returned, it waits for a signal to resume.
//
template <typename Stream>
awaitable<void> NGHTttp2SessionImpl<Stream>::send_loop()
{
   Buffer buffer;
   buffer.reserve(1460);
   for (;;)
   {
      //
      // Retrieve a chunk of data from NGHTTP2, to be sent.
      //
      const uint8_t* data; // data is valid until next call, so we don't need to copy it

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
      else if (const auto to_write = buffer.size() + nread; to_write > 0)
      {
         const std::array<asio::const_buffer, 2> seq{buffer.data(), asio::buffer(data, nread)};
         mylogd("send loop: writing {} bytes...", to_write);
         auto [ec, written] = co_await asio::async_write(m_stream, seq, as_tuple(deferred));
         if (ec)
         {
            mloge("send loop: error writing {} bytes: {}", to_write, ec.message());
            break;
         }
         mylogd("send loop: writing {} bytes... done, wrote {}", to_write, written);
         assert(to_write == written);
         buffer.consume(written);
      }

      //
      // Finally, if there is really nothing to send any more, wait for a signal to start again.
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

   mylogd("send loop: done");
}

// -------------------------------------------------------------------------------------------------

//
// The read loop is better suited for implementation as a coroutine than the write loop,
// because it does not need to wait on re-activation by the user.
//
template <typename Stream>
awaitable<void> NGHTttp2SessionImpl<Stream>::recv_loop()
{
   m_buffer.reserve(64 * 1024);

   std::string reason("done");
   while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))
   {
      auto free = m_buffer.capacity() - m_buffer.size();
      auto [ec, n] = co_await m_stream.async_read_some(m_buffer.prepare(free), as_tuple(deferred));
      if (ec)
      {
         mylogd("read: {}, terminating session", ec.message());
         reason = ec.message();
         nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
         break;
      }
      m_buffer.commit(n);

      handle_buffer_contents();
      start_write();
   }

   if (reason == "done")
      nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);

   start_write();
   mlogi("recv loop: {}, served {} requests", reason, m_requestCounter);
}

// =================================================================================================

} // namespace anyhttp::nghttp2
