#include "anyhttp/client_impl.hpp"
#include "anyhttp/beast_session.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/detail/endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;
using namespace boost::asio;

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::client
{

// =================================================================================================

Request::Impl::Impl() noexcept { logd("\x1b[1;34mClient::Request: ctor\x1b[0m"); }
Request::Impl::~Impl() { logd("\x1b[34mClient::Request: dtor\x1b[0m"); }

Response::Impl::Impl() noexcept { logd("\x1b[1;34mClient::Response: ctor\x1b[0m"); }
Response::Impl::~Impl() { logd("\x1b[34mClient::Response: dtor\x1b[0m"); }

// =================================================================================================

Client::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
#if !defined(NDEBUG)
   spdlog::set_level(spdlog::level::debug);
#else
   spdlog::set_level(spdlog::level::info);
#endif
   spdlog::info("Client: ctor");
}

Client::Impl::~Impl() { logi("Client: dtor"); }

// -------------------------------------------------------------------------------------------------

awaitable<void> Client::Impl::connect(ConnectHandler handler)
{
   auto executor = co_await boost::asio::this_coro::executor;

   std::string host = config().url.host_address();
   std::string port = config().url.port();
   ip::tcp::resolver resolver(executor);
   auto flags = resolver.numeric_service;

   auto [ec, endpoints] = co_await resolver.async_resolve(host, port, flags, as_tuple(deferred));
   for (auto&& elem : endpoints)
      ; // logd("Client: {}:{} -> {}", elem.host_name(), elem.service_name(), elem.endpoint());

   ip::tcp::socket socket(executor);
   ip::tcp::endpoint endpoint;
   std::tie(ec, endpoint) = co_await asio::async_connect(socket, endpoints, as_tuple(deferred));

   logi("Client: connected to {} ({})", socket.remote_endpoint(), ec.message());

   using sb = boost::asio::socket_base;
   sb::send_buffer_size send_buffer_size;
   sb::receive_buffer_size receive_buffer_size;
   socket.get_option(send_buffer_size);
   socket.get_option(receive_buffer_size);
   logd("Client: socket buffer sizes: send={} receive={}", send_buffer_size.value(),
        receive_buffer_size.value());
#if 0
   socket.set_option(sb::send_buffer_size(8192));
   socket.set_option(sb::receive_buffer_size(8192)); // makes 'PostRange' testcases very slow
#endif

   //
   // detect HTTP2 client preface, abort connection if not found
   //
   std::vector<uint8_t> data;
   // data.reserve(std::max(data.size(), 16 * 1024UL));
   // auto buffer = boost::asio::dynamic_buffer(data);

   std::shared_ptr<Session::Impl> impl;
   switch (config().protocol)
   {
   case Protocol::http11:
      impl = std::make_shared<beast_impl::BeastSession>(*this, executor, std::move(socket));
      break;
   case Protocol::http2:
      impl = std::make_shared<nghttp2::NGHttp2Session>(*this, executor, std::move(socket));
      break;
   };

   auto session = Session{impl};

#if 1
   co_spawn(executor, impl->do_client_session(std::move(data)),
            [session = std::move(session)](const std::exception_ptr& ex)
            {
               if (ex)
                  logw("client run: {}", what(ex));
               else
                  logi("client run: done");
            });

   //
   // It's important to call this handler after spawning the session, because stream configuration
   // happens in the beginning of do_client_session(), but calling the handler may result in a
   // submit(), which must happen after that.
   //
   // FIXME: instead of executing a submit() directly, it should be queued and executed within
   //        do_client_session(). This approach also avoids the problem, and allows pipelining, too.
   //
   std::move(handler)(boost::system::error_code{}, session);
   handler = nullptr;
#else
   co_await impl->do_client_session(std::move(data));

   std::move(handler)(boost::system::error_code{}, Session{impl});
   handler = nullptr;
#endif
}

void Client::Impl::async_connect(ConnectHandler&& handler)
{
   co_spawn(m_executor, connect(std::move(handler)), detached);
}

// =================================================================================================

} // namespace anyhttp::client
