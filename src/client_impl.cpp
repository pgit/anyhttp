#include "anyhttp/client_impl.hpp"
#include "anyhttp/alt_svc.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h1/backend.hpp"
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h3/backend.hpp"

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

namespace
{

bool is_https(const boost::urls::url& url) { return url.scheme_id() == boost::urls::scheme::https; }

} // namespace

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

Task<std::vector<TcpEndpoint>> Client::Impl::resolve(std::string host, std::string port)
{
   mlogd("resolving {}:{} ...", host, port);
   auto [ec, endpoints] = co_await io::resolve(executor_, host, port);
   if (ec)
      throw_error(ec);
   for (auto& endpoint : endpoints)
      mlogd("{}:{} -> {}", host, port, endpoint);
   co_return std::move(endpoints);
}

Task<Session> Client::Impl::async_connect()
{
   //
   // Where the connection goes is up to the caller, if the caller has said so, and then nothing
   // that is to be learned about the host of the URL matters.
   //
   std::vector<TcpEndpoint> endpoints = config().endpoints;
   const std::string host = config().url.host_address();

   if (endpoints.empty())
   {
      std::string port = config().url.port();

      //
      // A URL without a port means the default port of what it is going to be spoken over. HTTP/3
      // is TLS whatever the scheme says.
      //
      if (port.empty())
         port = config().protocol == Protocol::h3 || is_https(config().url) ? "443" : "80";

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
         endpoints = co_await resolve(alt_host, alt->port);
         co_return Session{
            co_await h3::async_connect_http3(executor_, std::move(endpoints), config())};
      }

      endpoints = co_await resolve(host, port);
   }

   //
   // HTTP/3 runs over QUIC (UDP), so it needs an entirely different transport setup (TLS,
   // handshake, ...) than the TCP-based h1/h2 paths below.
   //
   if (config().protocol == Protocol::h3)
      co_return Session{
         co_await h3::async_connect_http3(executor_, std::move(endpoints), config())};

   //
   // Initiate connection.
   //
   auto socket = io::make_socket(executor_);
   auto [connect_ec, endpoint] = co_await io::connect(socket, std::move(endpoints));
   if (connect_ec)
      throw_error(connect_ec);

   mlogi("connected to {}", endpoint);

   const auto prefix = anyhttp::log_prefix(Role::client, "tcp", io::remote_endpoint(socket));

   io::no_delay(socket);

   //
   // After connect(), not before: the socket a client starts with is not open yet, and
   // boost::asio::async_connect() closes and reopens it for every endpoint it tries, which would
   // throw away anything set on it. Zero, the default, leaves the kernel's autotuning alone; see
   // Config::socket_send_buffer_size for why a test would want them pinned.
   //
   io::set_buffer_sizes(socket, config().socket_send_buffer_size,
                        config().socket_receive_buffer_size);

   // What the kernel made of the sizes set above, or its own if they were left at zero.
   auto [send_buffer_size, receive_buffer_size] = io::buffer_sizes(socket);
   logd("[{}] socket buffer sizes: send={} receive={}", prefix, send_buffer_size,
        receive_buffer_size);

   //
   // The scheme alone decides about TLS, so that an "https" URL can never end up in cleartext. The
   // certificate has to be valid for the host of the URL, wherever the connection went.
   //
   if (is_https(config().url))
   {
      auto tls_context = make_client_tls_context(config().tls_ca_file, config().protocol);
      auto tls = io::make_tls_stream(std::move(socket), tls_context);
      io::set_peer_name(tls, host);
      if (auto [ec] = co_await io::handshake(tls, Role::client); ec)
      {
         logw("[{}] TLS handshake: {}", prefix, ec.message());
         throw_error(ec);
      }

      //
      // Anything but "h2" is HTTP/1.1, as on the server: that includes a server that does not use
      // ALPN at all, with which HTTP/2 is not an option (RFC 9113, section 3.2).
      //
      const auto alpn = io::alpn(tls);
      logi("[{}] connected over TLS ({})", prefix, io::tls_info(tls));
      co_return start_session(alpn == "h2" ? Protocol::h2 : Protocol::h1, std::move(tls));
   }

   co_return start_session(config().protocol, std::move(socket));
}

// -------------------------------------------------------------------------------------------------

template <SocketStream Stream>
Session Client::Impl::start_session(Protocol protocol, Stream&& stream)
{
   // what the session is going to call itself, see make_client_session()
   const auto prefix = anyhttp::log_prefix(Role::client,
                                           protocol == Protocol::h1 ? "h1"
                                           : is_tls(stream)         ? "h2"
                                                                    : "h2c",
                                           io::remote_endpoint(stream));

   //
   // Select implementation, by configuration or, with TLS, what ALPN has come up with.
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
   switch (protocol)
   {
   case Protocol::h1:
      impl = h1::make_client_session(*this, executor_, std::forward<Stream>(stream));
      break;

   case Protocol::h2:
      impl = h2::make_client_session(*this, executor_, std::forward<Stream>(stream));
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
   return Session{std::move(impl)};
}

// =================================================================================================

} // namespace anyhttp::client
