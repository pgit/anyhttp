#include "anyhttp/server_impl.hpp"

#include "anyhttp/detail/detect.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h1_backend.hpp"
#include "anyhttp/h2_backend.hpp"
#include "anyhttp/h3_backend.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include <boost/beast/core/flat_buffer.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <span>
#include <string_view>

using namespace std::chrono_literals;

namespace anyhttp::server
{

// =================================================================================================

#if 0
Request::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Request: ctor\x1b[0m"); }
Request::Impl::~Impl() { logd("\x1b[35mServer::Request: dtor\x1b[0m"); }

Response::Impl::Impl() noexcept { logd("\x1b[1;35mServer::Response: ctor\x1b[0m"); }
Response::Impl::~Impl() { logd("\x1b[35mServer::Response: dtor\x1b[0m"); }
#else
Request::Impl::Impl() noexcept = default;
Request::Impl::~Impl() = default;

Response::Impl::Impl() noexcept = default;
Response::Impl::~Impl() = default;
#endif

// =================================================================================================

Server::Impl::Impl(Executor executor, Config config)
   : config_(std::move(config)), executor_(std::move(executor)),
     tls_context_(make_server_tls_context(config_.tls_certificate_chain, config_.tls_private_key)),
     acceptor_(io::make_acceptor(executor_))
{
   mlogi("ctor");
   if (config_.use_strand && !multithreaded_runtime)
      throw std::invalid_argument("Config::use_strand: this runtime runs on one thread only");
   listen_tcp();

   //
   // HTTP/3 shares the endpoint the TCP acceptor is listening on, so it has to be set up after
   // listen_tcp(): with port=0 the actual port is only known once the acceptor is bound.
   //
   auto tcp_ep = io::local_endpoint(acceptor_);
   http3_ = make_http3_server(*this, asio::ip::udp::endpoint{tcp_ep.address(), tcp_ep.port()});

   //
   // Advertise that endpoint to HTTP/1.1 and HTTP/2 clients, see Config::alt_svc_max_age. The
   // alt-authority carries the port alone: an empty host in one means the host of the origin
   // itself, which is exactly where HTTP/3 is, one transport over.
   //
   if (http3_ && config_.alt_svc_max_age > 0s)
   {
      alt_svc_ = std::format("h3=\":{}\"; ma={}", tcp_ep.port(), config_.alt_svc_max_age.count());
      mlogi("advertising '{}'", alt_svc_);
   }
}

// -------------------------------------------------------------------------------------------------

/**
 * A shared pointer is captured in the completion handler of the spawned tasks. This way, we
 * make sure it stays around long enough, even if the user has already deleted it.
 *
 * Most of the cleanup is done at the end of listen_loop(), which collects all the shared pointers.
 */
void Server::Impl::start()
{
   launch(executor_, tcp_accept_loop(), [self = shared_from_this()](const std::exception_ptr& ex) {
      if (ex)
         logw("[{}] TCP accept loop: {}", self->log_prefix(), what(ex));
      else
         logi("[{}] TCP accept loop: done", self->log_prefix());
   });

   if (http3_)
      http3_->start();
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::destroy()
{
   mlogi("destroy");

   io::close(acceptor_); // breaks tcp_accept_loop()

   //
   // Destroy all active sessions (TCP and QUIC) so their timers and async operations are
   // cancelled, allowing the io_context to drain. QUIC sessions send a final CONNECTION_CLOSE
   // as part of destroy() -- through their own dup()ed fd, so closing the shared UDP socket
   // below doesn't race with it. Setting destroyed_ under the same lock is what keeps
   // process_quic_batch(), running on some session strand, from registering a new session
   // after this loop has run: it re-checks the flag under the lock before inserting.
   //
   {
      auto lock = std::lock_guard(session_mutex_);
      destroyed_ = true;
      for (auto& session : sessions_)
         session->destroy();
   }

   if (http3_)
      http3_->destroy();
}

// -------------------------------------------------------------------------------------------------

Server::Impl::~Impl()
{
   mlogi("dtor");
   assert(destroyed_);
}

// -------------------------------------------------------------------------------------------------

bool Server::Impl::add_session(std::shared_ptr<Session::Impl> session)
{
   auto lock = std::lock_guard(session_mutex_);
   if (destroyed_)
      return false;
   sessions_.emplace(std::move(session));
   return true;
}

void Server::Impl::remove_session(const std::shared_ptr<Session::Impl>& session)
{
   auto lock = std::lock_guard(session_mutex_);
   sessions_.erase(session);
}

// =================================================================================================

void Server::Impl::listen_tcp()
{
   boost::system::error_code ec; // what Boost.Asio's address parser reports in, either way
   auto address = asio::ip::make_address(config().listen_address, ec);
   if (ec)
      mlogw("error resolving '{}': {}", config().listen_address, ec.message());

   io::listen(acceptor_, asio::ip::tcp::endpoint(address, config().port));
   mlogi("TCP listening on {}", io::local_endpoint(acceptor_));
}

// =================================================================================================

//
// The TLS context every TCP connection is served from is created once, when the server is
// constructed, and not per connection: building it reads the PEM files from disk, and a context
// built per connection would also pick up a certificate that was rotated underneath a running
// server -- unlike HTTP/3, which holds its context for the lifetime of the server. That
// difference made a regenerated test PKI fail over HTTP/3 while HTTP/2 silently kept working.
//

// -------------------------------------------------------------------------------------------------

Task<void> Server::Impl::handle_connection(TcpSocket socket)
{
   const auto prefix = anyhttp::log_prefix(Role::server, "tcp", io::remote_endpoint(socket));
   logi("[{}] new connection", prefix);

   io::no_delay(socket);

   // Playing with socket buffer sizes doesn't seem to do any good: 8 KiB of receive buffer makes
   // the 'PostRange' testcases very slow, for example.
   auto [send_buffer_size, receive_buffer_size] = io::buffer_sizes(socket);
   logd("[{}] socket buffer sizes: send={} receive={}", prefix, send_buffer_size,
        receive_buffer_size);

   auto buffer = boost::beast::flat_buffer();
   auto [ec, detected] = co_await detail::detect(socket, buffer);
   if (ec)
   {
      logi("[{}] detecting protocol: {}", prefix, ec.message());
      co_return;
   }

   std::shared_ptr<Session::Impl> session;
   if (detected == detail::Detected::tls)
   {
      logi("[{}] detected TLS", prefix);

      auto tls = io::make_tls_stream(std::move(socket), tls_context_);
      if (auto [ec] = co_await io::handshake(tls, Role::server); ec)
      {
         logi("[{}] TLS handshake: {}", prefix, ec.message());
         co_return;
      }

      const auto alpn = io::alpn(tls);
      logi("[{}] {}", prefix, io::tls_info(tls));

      //
      // Everything that is not "h2" is served as HTTP/1.1, including the empty ALPN of a client
      // that offered none at all (curl --no-alpn) and one nobody agreed on. Refusing those would
      // buy nothing: HTTP/1.1 is what a connection without a negotiated protocol speaks anyway.
      //
      if (alpn == "h2")
         session = nghttp2::make_server_session(*this, std::move(tls));
      else
         session = beast_impl::make_server_session(*this, std::move(tls));
   }

   else if (detected == detail::Detected::h2c)
   {
      logi("[{}] detected HTTP2 client preface, {} bytes in buffer", prefix, buffer.size());
      session = nghttp2::make_server_session(*this, make_plain_server_stream(std::move(socket)));
   }

   //
   // fallback to HTTP/1.1
   //
   else
   {
      logi("[{}] no HTTP2 client preface, assuming HTTP/1.x", prefix);
      session = beast_impl::make_server_session(*this, make_plain_server_stream(std::move(socket)));
   }

   //
   // Registration fails only if the server is already being destroyed, in which case this
   // session has to go away right here: nothing else knows about it any more.
   //
   if (!add_session(session))
   {
      logi("[{}] server is shutting down, dropping connection", prefix);
      session->destroy();
      co_return;
   }

   co_await session->do_session(std::move(buffer));
   remove_session(session);

   logd("[{}] session finished", prefix);
}

// -------------------------------------------------------------------------------------------------

/**
 * Typically, an accept loop "spawns" a new thread of execution for each connection it accepts.
 * Doing that in a "detached" fashion violates the principles of structured concurrency, as we
 * don't have a clear way of cancelling those threads.
 *
 * To solve this, we always use spawn with a callback and use that to wait for pending tasks.
 *
 * https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3149r5.html#listener-loop-in-an-http-server
 *
 */
Task<void> Server::Impl::tcp_accept_loop()
{

   //
   // FIXME: sessionCounter and sessions_ are not thread safe, yet
   //
   // The main problem with sessions_ is that the new session is emplaced within
   // handle_connection(), which is already outside this coroutines strand.
   //
   // Maybe the simplest solution is to put a mutex around it...
   //
   size_t sessionCounter = 0;

   //
   // Sessions run on their own executors and finish on whatever thread happens to be running
   // them, so the "one more is gone" signal has to cross threads: a Signal, not an Event. It is
   // only a nudge -- sessionCounter, read under the mutex, is the actual condition -- so a
   // notify() that finds one pending already may be dropped: whenever the waiter is about to
   // block, none is pending, and every session still counted has its own notify() ahead of it.
   //
   Signal sessionDone{executor_};

   for (;;)
   {
      //
      // Put each connection on a strand if needed. The socket is accepted onto that executor,
      // so that everything layered on top of it stays there, too: the session takes its executor
      // from the stream it is given, see the make_*_session() factories.
      //
      // NOTE: This is slow. Consider multiple IO contexts instead,
      //       or explicit thread pools where really needed.
      //
      auto connection_executor = config().use_strand ? new_strand(executor_) : executor_;
      auto socket = io::make_socket(connection_executor);
      auto [ec] = co_await io::accept(acceptor_, socket);
      if (ec)
      {
         // bad_descriptor: the acceptor was closed before async_accept() got to it
         if (ec == errc::operation_canceled || ec == errc::bad_file_descriptor)
            mlogi("TCP accept: {}", ec.message());
         else
            mlogw("TCP accept: {}", ec.message());
         break;
      }

      auto prefix = anyhttp::log_prefix(Role::server, "tcp", io::remote_endpoint(socket));

      //
      // Without something like a "nursery" or "async_scope", spawning a task detaches it from
      // the owning class without any means to join it. Here, we use a simple session counter to
      // track their lifetime.
      //
      {
         auto lock = std::lock_guard(session_mutex_);
         ++sessionCounter;
      }

      launch(connection_executor, handle_connection(std::move(socket)),
             [&, prefix](const std::exception_ptr& ex) mutable {
                auto lock = std::lock_guard(session_mutex_);
                --sessionCounter;
                sessionDone.notify();
                if (ex)
                   logw("[{}] {}", prefix, what(ex));
                else if (sessionCounter)
                   logd("[{}] session finished, {} sessions left", prefix, sessionCounter);
                else
                   logi("[{}] all sessions finished", prefix);
             });
   }

   //
   // Wait for the sessions spawned above, sweeping the registry on every wake-up: a connection
   // that was already in flight when the acceptor closed may still register itself after the
   // first sweep, and destroying it is what makes it finish.
   //
   auto lock = std::unique_lock(session_mutex_);
   const auto waitingFor = sessionCounter;
   mlogi("accept terminated, waiting for {} sessions...", waitingFor);

   size_t i = 0;
   for (; sessionCounter; ++i)
   {
      for (auto& session : sessions_)
         session->destroy();
      sessions_.clear();

      lock.unlock();
      std::ignore = co_await sessionDone.wait();
      lock.lock();
   }

   mlogi("accept terminated, waiting for {} sessions... done, {} iterations", waitingFor, i);
}

// =================================================================================================

} // namespace anyhttp::server
