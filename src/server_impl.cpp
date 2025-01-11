#include "anyhttp/server_impl.hpp"
#include "anyhttp/beast_session.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/detect_http2.hpp"

#include "anyhttp/beast_session.hpp"
#include "anyhttp/detail/nghttp2_session_details.hpp"
#include "anyhttp/nghttp2_session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/as_single.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <netinet/udp.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

// #include <netinet/ip.h>
#define IPTOS_ECN_MASK 0x03

#include "ngtcp2/shared.h"

using namespace std::chrono_literals;
using namespace boost::asio;

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

Server::Impl::Impl(boost::asio::any_io_executor executor, Config config)
   : m_config(std::move(config)), m_executor(std::move(executor)), m_acceptor(m_executor)
{
#if !defined(NDEBUG)
   spdlog::set_level(spdlog::level::info);
#else
   spdlog::set_level(spdlog::level::info);
#endif
   logi("Server: ctor");
   listen_tcp();
   listen_udp();
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
   co_spawn(m_executor, listen_loop(),
            [self = shared_from_this()](const std::exception_ptr& ex)
            {
               if (ex)
                  logw("TCP accept loop: {}", what(ex));
               else
                  logi("TCP accept loop: done");
            });

   if (m_udp_socket)
   {
      co_spawn(m_executor, udp_receive_loop(),
               [self = shared_from_this()](const std::exception_ptr& ex)
               {
                  if (ex)
                     logw("UDP receive loop: {}", what(ex));
                  else
                     logi("UDP receive loop: done");
               });
   }
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::destroy()
{
   logi("Server: destroy");

   if (m_acceptor)
      m_acceptor->close(); // breaks listen_loop()

   if (m_udp_socket)
      m_udp_socket->close(); // breaks udp_receive_loop()

   m_stopped = true;
}

// -------------------------------------------------------------------------------------------------

Server::Impl::~Impl()
{
   logi("Server: dtor");
   assert(m_stopped);
}

// =================================================================================================

void Server::Impl::listen_tcp()
{
   assert(m_acceptor);
   auto& acceptor = *m_acceptor;

   boost::system::error_code ec;
   auto address = ip::make_address(config().listen_address, ec);
   if (ec)
      logw("Server: error resolving '{}': {}", config().listen_address, ec.what());

   ip::tcp::endpoint endpoint(address, config().port);
   if (endpoint.protocol() == ip::tcp::v6())
      std::ignore = acceptor.set_option(ip::v6_only(false), ec);

   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(endpoint);
   acceptor.listen();

   endpoint = acceptor.local_endpoint();
   logi("Server: listening on {}", endpoint);
}

// -------------------------------------------------------------------------------------------------

void Server::Impl::listen_udp()
{
   //
   // QUIC test -- open a UDP port
   //
   m_udp_socket.emplace(m_executor, ip::udp::endpoint(ip::udp::v4(), config().port));
   m_udp_socket->set_option(boost::asio::detail::socket_option::integer<IPPROTO_IP, IP_RECVTOS>(1));
   m_udp_socket->set_option(boost::asio::detail::socket_option::integer<IPPROTO_IP, IP_RECVTTL>(1));
}

// =================================================================================================

//
// https://nghttp2.org/documentation/tutorial-server.html
//
static unsigned char next_proto_list[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

unsigned int next_proto_list_len = sizeof(next_proto_list);
static int next_proto_cb(SSL* s, const unsigned char** data, unsigned int* len, void* arg)
{
   *data = next_proto_list;
   *len = (unsigned int)next_proto_list_len;
   return SSL_TLSEXT_ERR_OK;
}

static int alpn_select_proto_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                const unsigned char* in, unsigned int inlen, void* arg)
{
   int rv = nghttp2_select_next_protocol((unsigned char**)out, outlen, in, inlen);
   switch (rv)
   {
   case 0:
      return SSL_TLSEXT_ERR_OK; // http/1.1
   case 1:
      return SSL_TLSEXT_ERR_OK; // h2
   case -1:
   default:
      return SSL_TLSEXT_ERR_NOACK;
   }
}

// -------------------------------------------------------------------------------------------------

awaitable<void> Server::Impl::handleConnection(ip::tcp::socket socket)
{
   const auto prefix = normalize(socket.remote_endpoint());
   logi("[{}] new connection", prefix);

   socket.set_option(ip::tcp::no_delay(true));

   //
   // Playing with socket buffer sizes... Doesn't seem to do any good.
   //
   using sb = boost::asio::socket_base;
   sb::send_buffer_size send_buffer_size;
   sb::receive_buffer_size receive_buffer_size;
   socket.get_option(send_buffer_size);
   socket.get_option(receive_buffer_size);
   logd("[{}] socket buffer sizes: send={} receive={}", prefix, send_buffer_size.value(),
        receive_buffer_size.value());
   // socket.set_option(sb::send_buffer_size(8192));
   // socket.set_option(sb::receive_buffer_size(8192)); // makes 'PostRange' testcases very slow

   auto executor = co_await boost::asio::this_coro::executor;

   auto buffer = boost::beast::flat_buffer();

   //
   // detect TLS
   //
   std::shared_ptr<Session::Impl> session;
   std::optional<asio::ssl::stream<asio::ip::tcp::socket>> sslStream;
   if (co_await async_detect_ssl_awaitable(socket, buffer, deferred))
   {
      logi("[{}] detected TLS client hello, {} bytes in buffer", prefix, buffer.size());

      asio::ssl::context ctx{asio::ssl::context::tlsv13};
      SSL_CTX_set_next_protos_advertised_cb(ctx.native_handle(), next_proto_cb, NULL);
      SSL_CTX_set_alpn_select_cb(ctx.native_handle(), alpn_select_proto_cb, NULL);
      ctx.use_certificate_chain_file("etc/darkbase-chain.pem");
      ctx.use_private_key_file("etc/darkbase-key.pem", asio::ssl::context::pem);

      sslStream.emplace(std::move(socket), ctx);
      auto n = co_await sslStream->async_handshake(asio::ssl::stream_base::server, buffer.data(),
                                                   asio::deferred);
      buffer.consume(n);

      //
      // perform ALPN
      //
      std::string_view alpn;
      {
         const unsigned char* data;
         unsigned int len;
         SSL_get0_alpn_selected(sslStream->native_handle(), &data, &len);
         if (data)
            alpn = std::string_view(reinterpret_cast<const char*>(data), len);
      }

      if (alpn == "h2")
         session =
            std::make_shared<nghttp2::ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>> //
            (*this, executor, std::move(*sslStream));
      else if (alpn == "http/1.1")
         session =
            std::make_shared<beast_impl::ServerSession<asio::ssl::stream<asio::ip::tcp::socket>>> //
            (*this, executor, std::move(*sslStream));
   }

   //
   // detect HTTP2 client preface
   //
   else if (co_await async_detect_http2_client_preface(socket, buffer, deferred))
   {
      logi("[{}] detected HTTP2 client preface, {} bytes in buffer", prefix, buffer.size());
      session = std::make_shared<nghttp2::ServerSession<asio::ip::tcp::socket>> //
         (*this, executor, std::move(socket));
   }

   //
   // fallback to HTTP/1.1
   //
   else
   {
      logi("[{}] no HTTP2 client preface, assuming HTTP/1.x", prefix);
      session = std::make_shared<beast_impl::ServerSession<boost::beast::tcp_stream>> //
         (*this, executor, boost::beast::tcp_stream(std::move(socket)));
   }


   {
      auto lock = std::lock_guard(m_sessionMutex);
      m_sessions.emplace(session);
   }

   co_await session->do_session(std::move(buffer));

   {
      auto lock = std::lock_guard(m_sessionMutex);
      m_sessions.erase(session);
   }

   logi("[{}] session finished", prefix);
}

// -------------------------------------------------------------------------------------------------

/**
 * Typically, a listen loop "spawns" a new thread of execution for each connection it accepts.
 * Doing that in a "detached" fassion violates the principles of structured concurrency, as we
 * don't have a clear way of cancelling those threads.
 *
 * To solve this, we always use spawn with a callback and use that to wait for pending tasks.
 *
 * https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3149r5.html#listener-loop-in-an-http-server
 *
 */
awaitable<void> Server::Impl::listen_loop()
{
   assert(m_acceptor);
   auto& acceptor = *m_acceptor;
   auto executor = co_await boost::asio::this_coro::executor;

   //
   // FIXME: sessionCounter and m_sessions are not thread safe, yet
   //
   // The main problem with m_sessions is that the new session is emplaced within
   // handleConnection(), which is already outside this couroutine's strand.
   //
   // Maybe the simplest solution is to put a mutex around it...
   //
   size_t sessionCounter = 0;
   for (;;)
   {
      auto [ec, socket] = co_await acceptor.async_accept(as_tuple(deferred));
      if (ec == boost::system::errc::operation_canceled)
      {
         logi("accept: {}", ec.message());
         break;
      }
      else if (ec)
      {
         logw("accept: {}", ec.message());
         break;
      }

      auto ep = normalize(socket.remote_endpoint());

      //
      // Without something like a "nursery" or "async_scope", spwaning a task detaches it from
      // the owning class without any means to join it. Here, we use a simple session counter to
      // track their destruction.
      //
      {
         auto lock = std::lock_guard(m_sessionMutex);
         ++sessionCounter;
      }

      // put each connection on a strand if needed
      if (config().use_strand)
         executor = boost::asio::make_strand(executor);

      co_spawn(
         executor,
         [this, socket = std::move(socket)]() mutable { //
            return handleConnection(std::move(socket));
         },
         [&](const std::exception_ptr& ex) mutable
         {
            auto lock = std::lock_guard(m_sessionMutex);
            --sessionCounter;
            if (ex)            
               logw("[{}] {}", ep, what(ex));
            else
               logi("[{}] session finished, {} sessions left", ep, sessionCounter);            
         });
   }

   auto lock = std::unique_lock(m_sessionMutex);
   const auto waitingFor = sessionCounter;
   logi("listen loop terminated, waiting for {} sessions...", waitingFor);

   while (sessionCounter)
   {
      for (auto& session : m_sessions)
         session->destroy(std::move(session));
      m_sessions.clear();

      lock.unlock();
      co_await post(executor, asio::deferred);
      lock.lock();
   }

   logi("listen loop terminated, waiting for {} sessions... done", waitingFor);
}

// =================================================================================================

boost::asio::ip::tcp::endpoint sockaddr_to_endpoint(const sockaddr_storage& addr)
{
   using namespace boost::asio::ip;
   if (addr.ss_family == AF_INET)
   {
      const auto& sa = reinterpret_cast<const sockaddr_in&>(addr);
      return tcp::endpoint(make_address_v4(ntohl(sa.sin_addr.s_addr)), ntohs(sa.sin_port));
   }
   else if (addr.ss_family == AF_INET6)
   {
      const auto& sa6 = reinterpret_cast<const sockaddr_in6&>(addr);
      const auto& bytes = reinterpret_cast<const address_v6::bytes_type&>(sa6.sin6_addr);
      return tcp::endpoint(make_address_v6(bytes, sa6.sin6_scope_id), ntohs(sa6.sin6_port));
   }
   else
   {
      throw std::invalid_argument("Unsupported address family");
   }
}

awaitable<void> Server::Impl::udp_receive_loop()
{
   std::array<uint8_t, 64 * 1024> buf;

   struct iovec iov{buf.data(), buf.size()};
   struct sockaddr_storage sender_addr;
   auto family = sender_addr.ss_family;
   struct msghdr msg{};
   msg.msg_name = &sender_addr;
   msg.msg_namelen = sizeof(sender_addr);
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   std::array<uint8_t, CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int))> control_data;
   msg.msg_control = control_data.data();
   msg.msg_controllen = control_data.size();

   for (;;)
   {
      co_await m_udp_socket->async_wait(boost::asio::socket_base::wait_read, deferred);
      // asio::ip::udp::endpoint remote_endpoint;
      // auto n =
      //         co_await m_udp_socket->async_receive_from(asio::buffer(buf), remote_endpoint,
      //         deferred);
      // logd("reived {} UDP bytes ({:02x} {:02x} {:02x} {:02x}", n, buf[0], buf[1], buf[2],
      // buf[3]);

      auto ec = recvmsg(m_udp_socket->native_handle(), &msg, 0);
      logd("ec={} from={} tos={}", ec, sockaddr_to_endpoint(sender_addr),
           ngtcp2::msghdr_get_ecn(&msg, family));

      for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
           cmsg = CMSG_NXTHDR(&msg, cmsg))
      {
         if (cmsg->cmsg_level == IPPROTO_IP)
         {
            if (cmsg->cmsg_type == IP_TOS)
            {
               int tos = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
               std::println("Received TOS: {:x}", tos);
            }
            else if (cmsg->cmsg_type == IP_TTL)
            {
               int ttl = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
               std::cout << "Received TTL: " << ttl << "\n";
            }
         }
         else if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO)
         {
            int gso_size = 0;
            memcpy(&gso_size, CMSG_DATA(cmsg), sizeof(gso_size));
            std::cout << "Received UDP GRO " << gso_size << "\n";
            break;
         }
      }
   }
}

// =================================================================================================

} // namespace anyhttp::server
