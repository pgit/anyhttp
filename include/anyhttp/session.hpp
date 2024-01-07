#pragma once

#include "common.hpp"  // IWYU pragma: keep
#include "server_impl.hpp"

#include <map>

#include <boost/asio.hpp>

#include "nghttp2/nghttp2.h"

using namespace std::chrono_literals;

using namespace boost::asio;
namespace asio = boost::asio;

using asio::awaitable;
using asio::co_spawn;

namespace anyhttp::server
{

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

#define mlogd(x, ...) logd("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)



class Stream;
class Session
{
public:
   explicit Session(Server::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket)
      : m_parent(parent), m_executor(std::move(executor)), m_socket(std::move(socket)),
        m_logPrefix(fmt::format("{}", normalize(m_socket.remote_endpoint())))
   {
      mlogd("session created");
      m_send_buffer.resize(64 * 1024);
   }

   ~Session()
   {
      nghttp2_session_del(session);
      mlogd("session destroyed");
   }

   awaitable<void> do_session(std::vector<uint8_t> data);
   awaitable<void> send_loop(stream& stream);
   awaitable<void> recv_loop(stream& stream);

   Server::Impl& parent() { return m_parent; }
   const auto& executor() const { return m_executor; }

   asio::any_completion_handler<void()> m_send_handler;

   template <asio::completion_token_for<void()> CompletionToken>
   auto async_wait_send(CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, void()>(
         [&](asio::completion_handler_for<void()> auto handler)
         {
            assert(!m_send_handler);

            auto work = boost::asio::make_work_guard(handler);
            m_send_handler = [handler = std::move(handler), work = std::move(work)]() mutable
            {
               auto alloc = boost::asio::get_associated_allocator(
                  handler, boost::asio::recycling_allocator<void>());

               asio::dispatch(
                  work.get_executor(),
                  asio::bind_allocator(alloc, [handler = std::move(handler)]() mutable { //
                     std::move(handler)();
                  }));
            };
         },
         token);
   }

   void create_stream(int stream_id)
   {
      m_streams.emplace(stream_id, std::make_shared<Stream>(*this, stream_id));
      m_requestCounter++;
   }

   Stream* find_stream(int32_t stream_id)
   {
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
         return it->second.get();
      else
         return nullptr;
   }

   auto close_stream(int32_t stream_id)
   {
      std::shared_ptr<Stream> stream;
      if (auto it = m_streams.find(stream_id); it != std::end(m_streams))
      {
         stream = it->second;
         m_streams.erase(it);
      }
      return stream;
   }

   void start_write()
   {
      if (!m_send_handler)
      {
         // logd("[{}] start_write: NO SEND HANDLER", m_logPrefix);
         return;
      }

      decltype(m_send_handler) handler;
      m_send_handler.swap(handler);
      logd("[{}] start_write: calling handler...", m_logPrefix);
      std::move(handler)();
      logd("[{}] start_write: calling handler... done", m_logPrefix);
   }

   const std::string& logPrefix() const { return m_logPrefix; }

public:
   nghttp2_session* session = nullptr;
   stream m_socket;

private:
   Server::Impl& m_parent;
   asio::any_io_executor m_executor;
   std::string m_logPrefix;

   std::vector<uint8_t> m_send_buffer;
   std::map<int, std::shared_ptr<Stream>> m_streams;

   size_t m_requestCounter = 0;
};

} // namespace anyhttp::server