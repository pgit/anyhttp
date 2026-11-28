#pragma once

//
// The network half of the runtime layer for capy/corosio, see anyhttp/net.hpp. asio/net.hpp
// defines the same names for Boost.Asio and has their documentation; what is said here is what
// differs.
//

#include "anyhttp/common.hpp"
#include "anyhttp/runtime.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/asio.hpp>
#include <boost/capy/ex/async_waker.hpp>
#include <boost/capy/write.hpp>

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

// the buffer vocabulary of the generic and protocol code stays Boost.Asio's
#include <boost/asio/buffer.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/socket.h>

namespace anyhttp::inline corosio
{

// =================================================================================================

using TcpSocket = boost::corosio::tcp_socket;
using TcpAcceptor = boost::corosio::tcp_acceptor;
using TlsContext = boost::corosio::tls_context;
using UdpSocket = boost::corosio::udp_socket;

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

   /**
    * corosio's TLS stream encrypts only the first buffer of a sequence, into a record of its own
    * that it sends right away. A response head is a dozen small buffers, and would go out as as
    * many records and send() calls. This copies small buffers into one first, as Boost.Asio's TLS
    * stream does: a single buffer, or a first one that would fill the copy, goes out as it is.
    *
    * The copy is a member, not on the stack as with Asio: corosio retries the encryption from it
    * after it has waited for the socket.
    */
   template <typename ConstBufferSequence>
   auto write_some(const ConstBufferSequence& buffers)
   {
      return tls_.write_some(linearize(buffers));
   }

   TcpSocket& socket() noexcept { return *socket_; }
   boost::corosio::openssl_stream& tls() noexcept { return tls_; }

private:
   /// The most a TLS record holds: anything that does not fill one is worth a copy.
   static constexpr size_t linearize_size = 16384;

   template <typename ConstBufferSequence>
   capy::const_buffer linearize(const ConstBufferSequence& buffers)
   {
      size_t used = 0;
      for (auto it = capy::buffer_sequence_begin(buffers), end = capy::buffer_sequence_end(buffers);
           it != end && used < linearize_size;)
      {
         capy::const_buffer buffer = *it++;
         if (buffer.size() == 0)
            continue;
         if (used == 0 && (it == end || buffer.size() >= linearize_size))
            return buffer;
         if (!linearized_)
            linearized_ = std::make_unique<std::byte[]>(linearize_size);
         const auto n = std::min(buffer.size(), linearize_size - used);
         std::memcpy(linearized_.get() + used, buffer.data(), n);
         used += n;
      }
      return {linearized_.get(), used};
   }

   std::unique_ptr<TcpSocket> socket_;
   boost::corosio::openssl_stream tls_;
   std::unique_ptr<std::byte[]> linearized_;
};

#define ANYHTTP_SERVER_STREAMS(X) X(::anyhttp::TcpSocket) X(::anyhttp::TlsStream)
#define ANYHTTP_CLIENT_STREAMS(X) X(::anyhttp::TcpSocket) X(::anyhttp::TlsStream)

// -------------------------------------------------------------------------------------------------

// I/O on the streams the sessions run on, see asio/net.hpp.
namespace io
{

template <typename Stream>
auto read_some(Stream& stream, boost::asio::mutable_buffer buffer)
{
   return stream.read_some(capy::mutable_buffer(buffer.data(), buffer.size()));
}

template <typename Stream, typename ConstBufferSequence>
auto write(Stream& stream, const ConstBufferSequence& buffers)
{
   return capy::write(stream, capy::from_asio(buffers));
}

/// Peeks at what has arrived on \p socket, without taking it. corosio has no flags for its
/// receive operations, so this waits for readiness and peeks at the native socket.
template <typename Socket>
IoTask<size_t> peek(Socket& socket, boost::asio::mutable_buffer buffer)
{
   for (;;)
   {
      if (auto [ec] = co_await socket.wait(boost::corosio::wait_type::read); ec)
         co_return std::tuple{ec, size_t{0}};

      auto n = ::recv(socket.native_handle(), buffer.data(), buffer.size(), MSG_PEEK);
      if (n > 0)
         co_return std::tuple{error_code{}, static_cast<size_t>(n)};
      if (n == 0)
         co_return std::tuple{errors::eof, size_t{0}};
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
         co_return std::tuple{last_error(), size_t{0}};
   }
}

/// Receives a datagram into \p buffer, on a connected datagram socket. A coroutine, unlike
/// read_some(): corosio's datagram operations do not copy their buffer sequence, as its stream
/// operations do, but point to it, so it has to outlive the operation.
template <typename Socket>
IoTask<size_t> receive(Socket& socket, boost::asio::mutable_buffer buffer)
{
   const capy::mutable_buffer buffers(buffer.data(), buffer.size());
   auto [ec, n] = co_await socket.recv(buffers);
   co_return std::tuple{ec, n};
}

/// Waits until \p socket has something to read, for a caller that reads it by hand.
template <typename Socket>
auto wait_readable(Socket& socket)
{
   return socket.wait(boost::corosio::wait_type::read);
}

} // namespace io

// -------------------------------------------------------------------------------------------------

template <typename Stream>
struct stream_traits;

template <>
struct stream_traits<TcpSocket>
{
   static TcpSocket& get_socket(TcpSocket& stream) noexcept { return stream; }
   static bool is_tls(const TcpSocket&) noexcept { return false; }
};

template <>
struct stream_traits<TlsStream>
{
   static TcpSocket& get_socket(TlsStream& stream) noexcept { return stream.socket(); }
   static bool is_tls(const TlsStream&) noexcept { return true; }
};

/// Unlike ASIO's, a stream tells no executor: a corosio socket knows its context, but not the
/// strand it is used on. The session factories are given theirs.
template <typename Stream>
concept SocketStream = requires(Stream& stream) {
   { stream_traits<Stream>::get_socket(stream) } -> std::same_as<TcpSocket&>;
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

// -------------------------------------------------------------------------------------------------

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
   return get_socket(stream).shutdown(what == Shutdown::receive ? boost::corosio::shutdown_receive
                                      : what == Shutdown::send  ? boost::corosio::shutdown_send
                                                                : boost::corosio::shutdown_both);
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
std::optional<TcpEndpoint> remote_endpoint(Stream& stream) noexcept
{
   auto& socket = get_socket(stream);
   if (!socket.is_open())
      return std::nullopt;
   auto endpoint = socket.remote_endpoint();
   if (endpoint.port() == 0)
      return std::nullopt; // not connected
   return endpoint;
}

} // namespace io

// -------------------------------------------------------------------------------------------------

/// Sends the TLS "close_notify", waiting for the peer's for two seconds at most.
template <SocketStream Stream>
Task<error_code> async_teardown(Stream& stream)
{
   if constexpr (std::same_as<Stream, TlsStream>)
   {
      auto [ec] =
         co_await boost::corosio::timeout(stream.tls().shutdown(), std::chrono::seconds(2));
      co_return ec;
   }
   else
      co_return error_code{};
}

static_assert(SocketStream<TcpSocket>);
static_assert(SocketStream<TlsStream>);

// -------------------------------------------------------------------------------------------------

/**
 * The server's TLS context, see asio/net.hpp. corosio prefers our order for ALPN. It has no
 * keylog: a \p keylog_file is ignored, with a warning.
 */
TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key,
                                   std::span<const Protocol> protocols,
                                   const std::string& keylog_file = {});

/// A client's TLS context, see asio/net.hpp, and without a keylog, as the server's.
TlsContext make_client_tls_context(const std::string& ca_file, Protocol protocol,
                                   const std::string& keylog_file = {}, bool verify = true);

namespace io
{

inline TcpSocket make_socket(const Executor& executor) { return TcpSocket(executor.context()); }
inline TcpAcceptor make_acceptor(const Executor& executor)
{
   return TcpAcceptor(executor.context());
}

void listen(TcpAcceptor& acceptor, const TcpEndpoint& endpoint, bool reuse_port = false,
            bool dual_stack = false);

inline TcpEndpoint local_endpoint(const TcpAcceptor& acceptor) { return acceptor.local_endpoint(); }

inline void close(TcpAcceptor& acceptor) noexcept { acceptor.close(); }

inline auto accept(TcpAcceptor& acceptor, TcpSocket& socket) { return acceptor.accept(socket); }

/// As with ASIO, a failure here is not worth an error.
inline void no_delay(TcpSocket& socket)
{
   std::ignore = socket.set_option(boost::corosio::socket_option::no_delay(true));
}

std::pair<int, int> buffer_sizes(TcpSocket& socket);

void set_buffer_sizes(TcpSocket& socket, int send, int receive);

IoTask<std::vector<TcpEndpoint>> resolve(Executor executor, std::string host, std::string port);

/// Connects \p socket to the first of \p endpoints that takes it, see asio/net.hpp.
IoTask<TcpEndpoint> connect(TcpSocket& socket, std::vector<TcpEndpoint> endpoints,
                            std::optional<IpAddress> local = {}, uint16_t port = 0);

inline TlsStream make_tls_stream(TcpSocket&& socket, TlsContext& context)
{
   return TlsStream(std::move(socket), context);
}

/// corosio treats an address the way tls_set_peer_name() does.
inline void set_peer_name(TlsStream& stream, const std::string& host)
{
   stream.tls().set_hostname(host);
}

inline auto handshake(TlsStream& stream, Role role)
{
   return stream.tls().handshake(role == Role::server ? boost::corosio::tls_role::server
                                                      : boost::corosio::tls_role::client);
}

inline std::string_view alpn(TlsStream& stream) { return stream.tls().alpn_protocol(); }

/// corosio exposes no SSL*, so there is less to tell than with ASIO.
std::string tls_info(TlsStream& stream);

inline bool is_truncated(const error_code& ec) { return ec == capy::error::stream_truncated; }

inline UdpSocket make_udp_socket(const Executor& executor) { return UdpSocket(executor.context()); }

/// corosio opens every socket non-blocking.
inline error_code open(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept
{
   return socket.open(endpoint.address().is_v6() ? boost::corosio::family::v6
                                                 : boost::corosio::family::v4);
}

inline void cancel(UdpSocket& socket) noexcept { socket.cancel(); }
inline void close(UdpSocket& socket) noexcept { socket.close(); }

} // namespace io

// -------------------------------------------------------------------------------------------------

/**
 * capy's async_waker: notify() may come from any thread, and wakes up the waiter or is kept for
 * the next wait(), several of them as one. The waiter has to run on a strand, or on a context run
 * by one thread only.
 */
class Signal
{
public:
   explicit Signal(const Executor&) {}

   void notify() { waker_.wake(); }

   IoTask<> wait()
   {
      auto [ec] = co_await waker_.wait();
      co_return std::tuple{ec};
   }

private:
   capy::async_waker waker_;
};

// =================================================================================================

} // namespace anyhttp::inline corosio
