#pragma once

#include "common.hpp" // IWYU pragma: keep
#include "server_impl.hpp"

#include <boost/asio.hpp>

namespace anyhttp::server
{
namespace beast_impl
{

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

#define mlogd(x, ...) logd("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, m_logPrefix __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

class BeastSession
{
public:
   explicit BeastSession(Server::Impl& parent, asio::any_io_executor executor,
                    asio::ip::tcp::socket&& socket)
      : m_parent(parent), m_executor(std::move(executor)), m_socket(std::move(socket)),
        m_logPrefix(fmt::format("{}", normalize(m_socket.remote_endpoint())))
   {
      mlogd("session created");
      m_send_buffer.resize(64 * 1024);
   }

   ~BeastSession() { mlogd("streams deleted"); }

   asio::awaitable<void> do_session(std::vector<uint8_t> data);

   Server::Impl& parent() { return m_parent; }
   const auto& executor() const { return m_executor; }
   const std::string& logPrefix() const { return m_logPrefix; }

public:
   nghttp2_session* session = nullptr;
   stream m_socket;

private:
   Server::Impl& m_parent;
   asio::any_io_executor m_executor;
   std::string m_logPrefix;

   std::vector<uint8_t> m_send_buffer;

   size_t m_requestCounter = 0;
};

// =================================================================================================

} // namespace beast_impl
} // namespace anyhttp::server