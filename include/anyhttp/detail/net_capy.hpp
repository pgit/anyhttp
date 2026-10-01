#pragma once

//
// The network half of the runtime layer for capy/corosio, see anyhttp/net.hpp. detail/net_asio.hpp
// defines the same names for Boost.Asio and has their documentation; what is said here is what
// differs.
//

#include "anyhttp/common.hpp"
#include "anyhttp/runtime.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/corosio/tls_context.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace anyhttp
{

// =================================================================================================

using TcpSocket = corosio::tcp_socket;
using TcpAcceptor = corosio::tcp_acceptor;
using TlsContext = corosio::tls_context;

/**
 * TLS over a TCP socket. corosio's TLS stream erases the type of what it runs on, so it can not
 * hand the socket back for a shutdown or for its peer's address. This keeps the socket next to
 * it, on the heap, so that it stays where the TLS stream points to when this is moved.
 */
class TlsStream
{
public:
   TlsStream(TcpSocket&& socket, const TlsContext& context)
      : socket_(std::make_unique<TcpSocket>(std::move(socket))), tls_(socket_.get(), context)
   {
   }

   template <typename MutableBufferSequence>
   auto read_some(const MutableBufferSequence& buffers)
   {
      return tls_.read_some(buffers);
   }

   template <typename ConstBufferSequence>
   auto write_some(const ConstBufferSequence& buffers)
   {
      return tls_.write_some(buffers);
   }

   TcpSocket& socket() noexcept { return *socket_; }
   corosio::openssl_stream& tls() noexcept { return tls_; }

private:
   std::unique_ptr<TcpSocket> socket_;
   corosio::openssl_stream tls_;
};

/// Cleartext goes over the plain socket: there is no type-erased stream to exercise here.
using PlainServerStream = TcpSocket;

inline PlainServerStream make_plain_server_stream(TcpSocket&& socket) { return std::move(socket); }

#define ANYHTTP_SERVER_STREAMS(X) X(::anyhttp::TcpSocket) X(::anyhttp::TlsStream)
#define ANYHTTP_CLIENT_STREAMS(X) X(::anyhttp::TcpSocket)

// -------------------------------------------------------------------------------------------------

/// Boost.Asio's endpoint, which the API speaks, as corosio's.
inline corosio::endpoint to_corosio(const asio::ip::tcp::endpoint& endpoint)
{
   const auto address = endpoint.address();
   if (address.is_v4())
      return {corosio::ipv4_address(address.to_v4().to_bytes()), endpoint.port()};
   return {corosio::ipv6_address(address.to_v6().to_bytes(), address.to_v6().scope_id()),
           endpoint.port()};
}

/// corosio's endpoint as Boost.Asio's, which the API speaks.
inline asio::ip::tcp::endpoint to_asio(const corosio::endpoint& endpoint)
{
   const auto address = endpoint.address();
   if (address.is_v4())
      return {asio::ip::address_v4(address.to_v4().to_bytes()), endpoint.port()};
   const auto v6 = address.to_v6();
   return {asio::ip::address_v6(v6.to_bytes(), v6.scope_id()), endpoint.port()};
}

// -------------------------------------------------------------------------------------------------

template <typename Stream>
struct stream_traits;

template <>
struct stream_traits<TcpSocket>
{
   static TcpSocket& get_socket(TcpSocket& stream) noexcept { return stream; }
   static bool is_tls(const TcpSocket&) noexcept { return false; }

   /// corosio's I/O objects know their context, not an executor: there is only one per context.
   static Executor get_executor(TcpSocket& stream) noexcept
   {
      return static_cast<corosio::io_context&>(stream.context()).get_executor();
   }
};

template <>
struct stream_traits<TlsStream>
{
   static TcpSocket& get_socket(TlsStream& stream) noexcept { return stream.socket(); }
   static bool is_tls(const TlsStream&) noexcept { return true; }
   static Executor get_executor(TlsStream& stream) noexcept
   {
      return stream_traits<TcpSocket>::get_executor(stream.socket());
   }
};

template <typename Stream>
concept SocketStream = requires(Stream& stream) {
   { stream_traits<Stream>::get_socket(stream) } -> std::same_as<TcpSocket&>;
   { stream_traits<Stream>::get_executor(stream) } -> std::convertible_to<Executor>;
   { stream_traits<Stream>::is_tls(stream) } -> std::convertible_to<bool>;
};

template <SocketStream Stream>
TcpSocket& get_socket(Stream& stream) noexcept
{
   return stream_traits<Stream>::get_socket(stream);
}

template <SocketStream Stream>
bool is_tls(const Stream& stream) noexcept
{
   return stream_traits<Stream>::is_tls(stream);
}

/// Sends the TLS "close_notify", waiting for the peer's for two seconds at most.
template <SocketStream Stream>
Task<error_code> async_teardown(Stream& stream)
{
   if constexpr (std::same_as<Stream, TlsStream>)
   {
      auto [ec] = co_await corosio::timeout(stream.tls().shutdown(), std::chrono::seconds(2));
      co_return ec;
   }
   else
      co_return error_code{};
}

static_assert(SocketStream<TcpSocket>);
static_assert(SocketStream<TlsStream>);

// -------------------------------------------------------------------------------------------------

/// The server's TLS context, see detail/net_asio.hpp. corosio prefers our order for ALPN.
TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key);

namespace io
{

enum class Shutdown
{
   receive,
   send,
   both
};

template <SocketStream Stream>
error_code shutdown(Stream& stream, Shutdown what) noexcept
{
   return get_socket(stream).shutdown(what == Shutdown::receive ? corosio::shutdown_receive
                                      : what == Shutdown::send  ? corosio::shutdown_send
                                                                : corosio::shutdown_both);
}

template <SocketStream Stream>
error_code cancel(Stream& stream) noexcept
{
   get_socket(stream).cancel();
   return {};
}

template <SocketStream Stream>
error_code close(Stream& stream) noexcept
{
   get_socket(stream).close();
   return {};
}

template <SocketStream Stream>
std::optional<asio::ip::tcp::endpoint> remote_endpoint(Stream& stream) noexcept
{
   auto& socket = get_socket(stream);
   if (!socket.is_open())
      return std::nullopt;
   auto endpoint = socket.remote_endpoint();
   if (endpoint.port() == 0)
      return std::nullopt; // not connected
   return to_asio(endpoint);
}

inline TcpSocket make_socket(const Executor& executor) { return TcpSocket(executor.context()); }
inline TcpAcceptor make_acceptor(const Executor& executor)
{
   return TcpAcceptor(executor.context());
}

void listen(TcpAcceptor& acceptor, const asio::ip::tcp::endpoint& endpoint);

inline asio::ip::tcp::endpoint local_endpoint(const TcpAcceptor& acceptor)
{
   return to_asio(acceptor.local_endpoint());
}

inline void close(TcpAcceptor& acceptor) noexcept { acceptor.close(); }

inline auto accept(TcpAcceptor& acceptor, TcpSocket& socket) { return acceptor.accept(socket); }

void no_delay(TcpSocket& socket);
std::pair<int, int> buffer_sizes(TcpSocket& socket);

Task<std::tuple<error_code, std::vector<asio::ip::tcp::endpoint>>>
resolve(Executor executor, std::string host, std::string port);

Task<std::tuple<error_code, asio::ip::tcp::endpoint>>
connect(TcpSocket& socket, std::vector<asio::ip::tcp::endpoint> endpoints);

inline TlsStream make_tls_stream(TcpSocket&& socket, TlsContext& context)
{
   return TlsStream(std::move(socket), context);
}

inline auto handshake(TlsStream& stream, Role role)
{
   return stream.tls().handshake(role == Role::server ? corosio::tls_role::server
                                                      : corosio::tls_role::client);
}

inline std::string_view alpn(TlsStream& stream) { return stream.tls().alpn_protocol(); }

/// corosio exposes no SSL*, so there is less to tell than with ASIO.
std::string tls_info(TlsStream& stream);

} // namespace io

// -------------------------------------------------------------------------------------------------

/// With only one thread, an Event does, cleared again for the next wait.
class Signal
{
public:
   explicit Signal(const Executor&) {}

   void notify() { event_.set(); }

   Task<std::tuple<error_code>> wait()
   {
      auto [ec] = co_await event_.wait();
      event_.clear();
      co_return std::tuple{ec};
   }

private:
   Event event_;
};

// =================================================================================================

} // namespace anyhttp
