#include "anyhttp/client_impl.hpp"
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

Request::Impl::Impl() noexcept = default;
Request::Impl::~Impl() = default;
Response::Impl::Impl() noexcept = default;
Response::Impl::~Impl() = default;

// =================================================================================================

Client::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
   spdlog::set_level(spdlog::level::info);
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
      logi("Client: {}:{} -> {}", elem.host_name(), elem.service_name(), elem.endpoint());

   // if (endpoints.empty())
   //   co_return;

   ip::tcp::socket socket(executor);
   ip::tcp::endpoint endpoint;
   std::tie(ec, endpoint) = co_await asio::async_connect(socket, endpoints, as_tuple(deferred));

   logi("connected to {} ({})", socket.remote_endpoint(), ec.message());

   //
   // detect HTTP2 client preface, abort connection if not found
   //
   std::vector<uint8_t> data;
   auto buffer = boost::asio::dynamic_buffer(data);
 
   auto impl = std::make_shared<nghttp2::NGHttp2Session>(*this, executor, std::move(socket));
   std::move(handler)(boost::system::error_code{}, Session{impl});
   handler = nullptr;
   co_await impl->do_client_session(std::move(data));
}

void Client::Impl::async_connect(ConnectHandler&& handler)
{
   co_spawn(m_executor, connect(std::move(handler)), detached);
}

// =================================================================================================

} // namespace anyhttp::client
