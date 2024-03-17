#pragma once

#include "client_impl.hpp"
#include "nghttp2_stream.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <map>

#include <boost/asio.hpp>

#include "nghttp2/nghttp2.h"

using namespace std::chrono_literals;

using namespace boost::asio;

namespace anyhttp::nghttp2
{

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

class NGHttp2Stream;
class NGHttp2Session : public ::anyhttp::Session::Impl
{
   NGHttp2Session(std::string_view log, any_io_executor executor, ip::tcp::socket&& socket);

public:
   NGHttp2Session(server::Server::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   NGHttp2Session(client::Client::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   ~NGHttp2Session() override;

   void create_client_session();

   // ----------------------------------------------------------------------------------------------

   client::Request submit(boost::urls::url url, Fields headers) override;
   awaitable<void> do_server_session(std::vector<uint8_t> data) override;
   awaitable<void> do_client_session(std::vector<uint8_t> data) override;
   void cancel() override
   {
      boost::system::error_code ec;
      std::ignore = m_socket.shutdown(boost::asio::socket_base::shutdown_both, ec);
      logw("[{}] shutdown: {}", m_logPrefix, ec.message());
   }

   // ----------------------------------------------------------------------------------------------

   awaitable<void> send_loop(stream& stream);
   awaitable<void> recv_loop(stream& stream);

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

   asio::any_completion_handler<void()> m_send_handler;

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

      if (m_streams.empty() && m_client)
      {
         logi("[{}] last stream closed, terminating session...", m_logPrefix);
         nghttp2_session_terminate_session(session, NGHTTP2_STREAM_CLOSED);
      }

      return stream;
   }

   void start_write()
   {
      if (m_send_handler)
      {
         decltype(m_send_handler) handler;
         m_send_handler.swap(handler);
         logd("[{}] start_write: calling handler...", m_logPrefix);
         std::move(handler)();
         logd("[{}] start_write: calling handler... done", m_logPrefix);
      }
   }

   const std::string& logPrefix() const { return m_logPrefix; }

public:
   nghttp2_session* session = nullptr;
   stream m_socket;

private:
   server::Server::Impl* m_server = nullptr;
   client::Client::Impl* m_client = nullptr;
   asio::any_io_executor m_executor;
   std::string m_logPrefix;

   std::vector<uint8_t> m_send_buffer;
   std::map<int, std::shared_ptr<NGHttp2Stream>> m_streams;

   size_t m_requestCounter = 0;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
