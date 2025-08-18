#include "anyhttp/client_impl.hpp"
#include "anyhttp/beast_session.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include "anyhttp/detail/nghttp2_session_details.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/detail/endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>

#include <boost/system/system_error.hpp>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::client
{
using namespace asio::experimental::awaitable_operators;

// =================================================================================================

#if 0
Request::Impl::Impl() noexcept { logd("\x1b[1;34mClient::Request: ctor\x1b[0m"); }
Request::Impl::~Impl() { logd("\x1b[34mClient::Request: dtor\x1b[0m"); }

Response::Impl::Impl() noexcept { logd("\x1b[1;34mClient::Response: ctor\x1b[0m"); }
Response::Impl::~Impl() { logd("\x1b[34mClient::Response: dtor\x1b[0m"); }
#else
Request::Impl::Impl() noexcept = default;
Request::Impl::~Impl() = default;

Response::Impl::Impl() noexcept = default;
Response::Impl::~Impl() = default;
#endif

// =================================================================================================

Client::Impl::Impl(asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_resolver(m_executor)
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

void Client::Impl::async_connect(ConnectHandler handler)
{
   return asio::async_initiate<ConnectHandler, Connect>(
      asio::experimental::co_composed<Connect>(
         [this](auto state, asio::any_io_executor exnix) -> void
         {
            const auto ex = m_executor;
            // auto ex = state.get_io_executor();  // FAILS to register work properly
            // auto token = bind_executor(ex, as_tuple(deferred));  // not needed
            constexpr auto token = as_tuple(deferred);

            std::string host = config().url.host_address();
            std::string port = config().url.port();

            logd("Client: resolving {}:{} ...", host, port);
            auto flags = ip::tcp::resolver::numeric_service;
            auto [ec, endpoints] = co_await m_resolver->async_resolve(host, port, flags, token);
            if (ec)
            {
               loge("Client: resolving {}:{}:", host, port, ec.message());
               co_return {ec, Session(nullptr)};
            }

            for (auto&& elem : endpoints)
               logd("Client: {}:{} -> {}", elem.host_name(), elem.service_name(), elem.endpoint());

            // ip::tcp::socket socket(make_strand(ex));
            // asio::deferred_t::as_default_on_t<ip::tcp::socket> socket(executor);
            ip::tcp::socket socket(ex);
            ip::tcp::endpoint endpoint;
            std::tie(ec, endpoint) = co_await asio::async_connect(socket, endpoints, token);

            if (ec)
            {
               loge("Client: {}", ec.message());
               co_return {ec, Session(nullptr)};
            }

            logi("Client: connected to {} ({})", socket.remote_endpoint(), ec.message());

            //
            // Playing with socket buffer sizes... Doesn't seem to do any good.
            //
            using sb = asio::socket_base;
            sb::send_buffer_size send_buffer_size;
            sb::receive_buffer_size receive_buffer_size;
            socket.get_option(send_buffer_size);
            socket.get_option(receive_buffer_size);
            logd("Client: socket buffer sizes: send={} receive={}", send_buffer_size.value(),
                 receive_buffer_size.value());
#if 1
      // socket.set_option(sb::send_buffer_size(8192));
      // socket.set_option(sb::receive_buffer_size(8192)); // makes 'PostRange' testcases very slow
#endif

            //
            // Select implementation, currently by configuration only.
            // With TLS and ALPN, HTTP protocol negotiation can be automatic as well.
            //
            // FIXME: How to handle upgrades? This is a top-level responsibility of the client.
            //
            // There are different types of upgrades:
            //
            // 1) HTTP/1.1 to HTTP/2 via Connection: upgrade header
            // 2) HTTP/1.1 to HTTP/3 via Alt-Svc header
            // 3) Proactively connect using HTTP/1 (using TCP) and HTTP/3 (UDP) in parallel
            // 4) Support DNS HTTPS RR (serving the same purpose as Alt-Svc)
            //
            std::shared_ptr<Session::Impl> impl;
            switch (config().protocol)
            {
            case Protocol::http11:
               impl = std::make_shared<beast_impl::ClientSession<boost::beast::tcp_stream>>(
                  *this, ex, boost::beast::tcp_stream(std::move(socket)));
               break;

            case Protocol::h2:
               impl = std::make_shared<nghttp2::ClientSession<asio::ip::tcp::socket>>(
                  *this, ex, std::move(socket));
               break;

            case anyhttp::Protocol::h3:
               namespace errc
               = boost::system::errc;
               co_return {errc::make_error_code(errc::invalid_argument), Session{nullptr}};
            };

      //
      // FIXME: Do we really need to "run" a session here? For nghttp2, yes. For beast, not so much,
      //        as there is no communication outside the current request right now, but that may
      //        still come with pipelining.
      //
      //        We do need a user interface to stop sessions, though. This should be the destructor
      //        of the user-facing "Session" object. So we should use only the "impl" internally.
      //
#if 1
            co_spawn(ex, impl->do_session(Buffer{}),
                     [impl](const std::exception_ptr& ex) mutable
                     {
                        if (ex)
                           logw("client run: {}", what(ex));
                        else
                           logi("client run: done");
                        impl.reset();
                     });
#endif

            //
            // It's important to call this handler AFTER spawning the session, because stream
            // configuration happens in the beginning of do_client_session(), and we don't want to
            // start any request before that.
            //
            // FIXME: instead of executing a submit() directly, it should be queued and executed
            // within
            //        do_client_session(). This approach avoids the problem, and allows pipelining,
            //        too.
            //
            // std::move(handler)(boost::system::error_code{}, Session{std::move(impl)});
            co_return {boost::system::error_code{}, Session{std::move(impl)}};
         }),
      handler, m_executor);
}

// =================================================================================================

} // namespace anyhttp::client
