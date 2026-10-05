#include "anyhttp/client_impl.hpp"
#include "anyhttp/alt_svc.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h1/backend.hpp"
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h3/backend.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/scope/scope_exit.hpp>

#include <boost/system/system_error.hpp>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <utility>

using namespace std::chrono_literals;

namespace anyhttp::client
{

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

Client::Impl::Impl(Executor executor, Config config)
   : config_(std::move(config)), executor_(std::move(executor))
{
   mlogi("ctor");
}

Client::Impl::~Impl() { mlogi("dtor"); }

// -------------------------------------------------------------------------------------------------

void Client::Impl::on_alt_svc(std::string_view field_value)
{
   if (!config().follow_alt_svc)
      return;

   const auto alt_svc = parse_alt_svc(field_value);

   //
   // "clear" tells us to forget what we know, and so does an alternative with "ma=0": it expires
   // the moment it arrives (RFC 7838, section 3.1).
   //
   const auto* service = alt_svc.find("h3");
   if (alt_svc.clear || (service && service->max_age.count() == 0))
   {
      auto lock = std::lock_guard(alt_svc_mutex_);
      if (alt_svc_)
         mlogi("Alt-Svc: dropping the HTTP/3 alternative");
      alt_svc_.reset();
      return;
   }

   if (!service)
      return;

   mlogi("Alt-Svc: HTTP/3 at {}:{} for {}s", //
         service->host.empty() ? config().url.host_address() : service->host, service->port,
         service->max_age.count());

   auto lock = std::lock_guard(alt_svc_mutex_);
   alt_svc_ = AlternativeService{.host = service->host,
                                 .port = service->port,
                                 .expires = std::chrono::steady_clock::now() + service->max_age};
}

std::optional<Client::Impl::AlternativeService> Client::Impl::alt_svc() const
{
   auto lock = std::lock_guard(alt_svc_mutex_);
   if (alt_svc_ && alt_svc_->expires <= std::chrono::steady_clock::now())
      return std::nullopt; // its "ma" has run out; the next advertisement overwrites it

   return alt_svc_;
}

// -------------------------------------------------------------------------------------------------

//
//
#if ANYHTTP_ASIO
void Client::Impl::async_connect(ConnectHandler handler)
{
   //
   // We have to forward cancellation from the passed handler to the coroutine here. This works
   // by binding the cancellation slot from the handler to the intermediate completion handler
   // for co_spawn() we create here (a lambda).
   //
   auto slot = get_associated_cancellation_slot(handler);
   auto executor = get_associated_executor(handler);
   auto completion = [this, handler = std::move(handler)](const std::exception_ptr& ep,
                                                          Session session) mutable {
      if (ep)
         mloge("async_connect: {}", what(ep));
      std::move(handler)(code(ep), std::move(session));
   };

   co_spawn(get_executor(), async_connect(),
            bind_executor(executor, bind_cancellation_slot(slot, std::move(completion))));
}
#endif

Task<Session> Client::Impl::async_connect()
{
   //
   // Extract host and port from URL and resolve hostname.
   //
   std::string host = config().url.host_address();
   std::string port = config().url.port();

   //
   // An HTTP/3 alternative service a previous session was told about takes precedence over the
   // configured protocol -- that is what following it means, see Config::follow_alt_svc. The
   // origin does not change with it, only where it is reached: requests still go out with the
   // authority of config().url.
   //
   if (auto alt = alt_svc())
   {
      auto alt_host = alt->host.empty() ? host : alt->host;
      mlogi("connecting to {}:{} over HTTP/3, as advertised by Alt-Svc", alt_host, alt->port);
      co_return Session{co_await async_connect_http3(executor_, alt_host, alt->port, config())};
   }

   //
   // HTTP/3 runs over QUIC (UDP), so it needs an entirely different transport setup (TLS,
   // handshake, ...) than the TCP-based h1/h2 paths below.
   //
   if (config().protocol == Protocol::h3)
      co_return Session{co_await async_connect_http3(executor_, host, port, config())};

   mlogd("resolving {}:{} ...", host, port);
   auto [resolve_ec, endpoints] = co_await io::resolve(executor_, host, port);
   if (resolve_ec)
      throw_error(resolve_ec);
   for (auto& endpoint : endpoints)
      mlogd("{}:{} -> {}", host, port, endpoint);

   //
   // Initiate connection.
   //
   // TODO: TLS
   //
   auto socket = io::make_socket(executor_);
   auto [connect_ec, endpoint] = co_await io::connect(socket, std::move(endpoints));
   if (connect_ec)
      throw_error(connect_ec);

   mlogi("connected to {}", endpoint);

   // what the session is going to call itself, see make_client_session() below (no TLS yet)
   const auto prefix = anyhttp::log_prefix(
      Role::client, config().protocol == Protocol::h1 ? "h1" : "h2c", io::remote_endpoint(socket));

   io::no_delay(socket);

   // Playing with socket buffer sizes doesn't seem to do any good, see the server.
   auto [send_buffer_size, receive_buffer_size] = io::buffer_sizes(socket);
   logd("[{}] socket buffer sizes: send={} receive={}", prefix, send_buffer_size,
        receive_buffer_size);

   //
   // Select implementation, currently by configuration only.
   // With TLS and ALPN, HTTP protocol negotiation can be automatic as well.
   //
   // FIXME: How to handle upgrades? This is a top-level responsibility of the client.
   //
   // There are different types of upgrades:
   //
   // 1) HTTP/1.1 to HTTP/2 via Connection: upgrade header
   // 2) HTTP/1.1 or HTTP/2 to HTTP/3 via Alt-Svc -- implemented, see Config::follow_alt_svc and
   //    on_alt_svc() above, which the sessions feed
   // 3) Proactively connect using HTTP/1 (using TCP) and HTTP/3 (UDP) in parallel
   // 4) Support DNS HTTPS RR (serving the same purpose as Alt-Svc)
   //
   std::shared_ptr<Session::Impl> impl;
   switch (config().protocol)
   {
   case Protocol::h1:
      impl = beast_impl::make_client_session(*this, executor_, std::move(socket));
      break;

   case Protocol::h2:
      impl = nghttp2::make_client_session(*this, executor_, std::move(socket));
      break;

   case anyhttp::Protocol::h3:
      // handled above, before the TCP resolve/connect
      std::unreachable();
   };

   //
   // FIXME: Do we really need to "run" a session here? For nghttp2, yes. For beast, not so much,
   //        as there is no communication outside the current request right now, but that may
   //        still come with pipelining or if it is refactored to have the parser and serializer
   //        in the session object itself.
   //
   //        We do need a user interface to stop sessions, though. This should be the destructor
   //        of the user-facing "Session" object. So we should use only the "impl" internally.
   //
#if 1
   launch(executor_, impl->do_session(Buffer{}),
          [impl, prefix](const std::exception_ptr& ex) mutable {
             if (ex)
                logw("[{}] client run: {}", prefix, what(ex));
             else
                logi("[{}] client run: done", prefix);
             impl.reset();
          });
#endif

   //
   // It's important to call this handler AFTER spawning the session, because stream
   // configuration happens in the beginning of do_client_session(), and we don't want to
   // start any request before that.
   //
   // FIXME: instead of executing a submit() directly, it should be queued and executed
   // within do_client_session(). This approach avoids the problem, and allows pipelining, too
   //
   co_return Session{std::move(impl)};
}

// =================================================================================================

} // namespace anyhttp::client
