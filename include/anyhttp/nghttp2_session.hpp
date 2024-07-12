#pragma once

#include "client_impl.hpp"
#include "nghttp2_stream.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <map>

#include <boost/asio.hpp>
#include <span>

#include "nghttp2/nghttp2.h"

using namespace std::chrono_literals;

using namespace boost::asio;

namespace anyhttp::nghttp2
{

// =================================================================================================

class NGHttp2Stream;
class NGHttp2Session : public ::anyhttp::Session::Impl
{
   NGHttp2Session(std::string_view logPrefix, any_io_executor executor);

protected:
   NGHttp2Session(server::Server::Impl& parent, any_io_executor executor);
   NGHttp2Session(client::Client::Impl& parent, any_io_executor executor);

public:
   ~NGHttp2Session() override;

   void create_client_session();

   // ----------------------------------------------------------------------------------------------

   void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) override;

   awaitable<void> do_server_session(Buffer&& data) override;
   awaitable<void> do_client_session(Buffer&& data) override;

   // ----------------------------------------------------------------------------------------------

   virtual awaitable<void> send_loop() = 0;
   virtual awaitable<void> recv_loop() = 0;

   // ----------------------------------------------------------------------------------------------

   /**
    * Helper function to pass data from #m_buffer to nghttp2, invokedby recv_loop().
    * The buffer will be empty when this function returns. Terminates the session on error.
    */
   void handle_buffer_contents();

   server::Server::Impl& server()
   {
      assert(m_server);
      return *m_server;
   }

   client::Client::Impl& client()
   {
      assert(m_client);
      return *m_client;
   }

   const auto& executor() const { return m_executor; }

   // ----------------------------------------------------------------------------------------------

   // If set, the send loop has run out of data to send and is waiting for re-activation.
   asio::any_completion_handler<void()> m_send_handler;

   // Wait to be resumed via `start_write()`, called from within `send_loop()`.
   template <asio::completion_token_for<void()> CompletionToken>
   auto async_wait_send(CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, void()>(
         [&](asio::completion_handler_for<void()> auto handler)
         {
            assert(!m_send_handler);
            m_send_handler = std::move(handler);
         },
         token);
   }

   // ----------------------------------------------------------------------------------------------

   void create_stream(int stream_id)
   {
      m_streams.emplace(stream_id, std::make_shared<NGHttp2Stream>(*this, stream_id));
      m_requestCounter++;
   }

   NGHttp2Stream* find_stream(int32_t stream_id)
   {
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
         return it->second.get();
      else
         return nullptr;
   }

   auto close_stream(int32_t stream_id)
   {
      std::shared_ptr<NGHttp2Stream> stream;
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
      {
         stream = it->second;
         m_streams.erase(it);
      }

      //
      // FIXME: We can't just terminate the session after the last request -- what if the user wants
      //        to do another one? Shutting down a session has to be (somewhat) explicit. Try to
      //        tie this to the lifetime of the user-facing 'Session' object...
      //
      if (m_client && m_streams.empty())
      {
         logi("[{}] last stream closed, terminating session...", m_logPrefix);
         nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
      }

      return stream;
   }

   void start_write()
   {
      if (m_send_handler)
      {
         decltype(m_send_handler) handler;
         m_send_handler.swap(handler);
         logd("[{}] start_write: signalling write loop...", m_logPrefix);
         std::move(handler)();
         logd("[{}] start_write: signalling write loop... done", m_logPrefix);
      }
   }

   const std::string& logPrefix() const { return m_logPrefix; }

public:
   nghttp2_session* session = nullptr;
   Buffer m_buffer;

protected:
   server::Server::Impl* m_server = nullptr;
   client::Client::Impl* m_client = nullptr;
   boost::asio::any_io_executor m_executor;
   std::string m_logPrefix;
   std::map<int, std::shared_ptr<NGHttp2Stream>> m_streams;
   size_t m_requestCounter = 0;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class NGHTttp2SessionImpl : public NGHttp2Session
{
public:
   NGHTttp2SessionImpl(server::Server::Impl& parent, any_io_executor executor, Stream&& stream)
      : NGHttp2Session(parent, executor), m_stream(std::move(stream))
   {
   }

   NGHTttp2SessionImpl(client::Client::Impl& parent, any_io_executor executor, Stream&& stream)
      : NGHttp2Session(parent, executor), m_stream(std::move(stream))
   {
   }

   void destroy() override;

   awaitable<void> send_loop() override;
   awaitable<void> recv_loop() override;

private:
   // asio::as_tuple_t<asio::deferred_t>::as_default_on_t<Stream> m_stream;
   Stream m_stream;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
