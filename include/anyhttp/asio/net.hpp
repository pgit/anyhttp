#pragma once

//
// The network half of the runtime layer for Boost.Asio, see anyhttp/net.hpp.
//
// The sessions are templates over the stream they run on, and there are four of those: a plain
// TCP socket, a TLS stream on top of one, beast's tcp_stream and the type-erased any_async_stream.
// Beyond the async read and write operations, which all of them have in common already, a session
// needs three more things from its stream: the underlying socket, to shut it down or close it, an
// executor to run its loops on, and whether it is encrypted (which tells h2 from h2c). None of them
// is spelled the same way by all four, so they are reached through this trait instead. Ending the
// stream itself, which only TLS has to do, is async and comes as a free function below.
//

#include "anyhttp/asio/any_async_stream.hpp"
#include "anyhttp/runtime.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <chrono>
#include <concepts>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace anyhttp
{

// =================================================================================================

using TcpSocket = asio::ip::tcp::socket;
using TcpAcceptor = asio::ip::tcp::acceptor;
using TlsContext = asio::ssl::context;
using TlsStream = asio::ssl::stream<TcpSocket>;
using UdpSocket = asio::ip::udp::socket;

/// What the server serves cleartext HTTP/1.1 and h2c over: for ASIO, the type-erased stream,
/// which keeps it exercised.
using PlainServerStream = any_async_stream;

inline PlainServerStream make_plain_server_stream(TcpSocket&& socket)
{
   return make_any_async_stream(std::move(socket));
}

//
// The stream types the backends instantiate their session factories for, as X-macros: the
// factory templates are defined in one source file per backend and explicitly instantiated there,
// and these keep the lists out of the backends.
//
#define ANYHTTP_SERVER_STREAMS(X)                                                                  \
   X(::anyhttp::TcpSocket) X(::anyhttp::TlsStream) X(::anyhttp::any_async_stream)
#define ANYHTTP_CLIENT_STREAMS(X) X(::anyhttp::TcpSocket)

// -------------------------------------------------------------------------------------------------

/**
 * Specialized below for every stream type a session can be instantiated with. The primary template
 * is left undefined on purpose, so that \c SocketStream rejects anything else.
 */
template <typename Stream>
struct stream_traits;

/// A plain TCP socket is its own socket.
template <typename Protocol, typename Executor>
struct stream_traits<boost::asio::basic_stream_socket<Protocol, Executor>>
{
   using stream_type = boost::asio::basic_stream_socket<Protocol, Executor>;

   static stream_type& get_socket(stream_type& stream) noexcept { return stream; }
   static Executor get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
   static bool is_tls(const stream_type&) noexcept { return false; }
};

/// A TLS stream, which may be layered on top of anything that has a socket at the bottom.
template <typename Layer>
struct stream_traits<boost::asio::ssl::stream<Layer>>
{
   using stream_type = boost::asio::ssl::stream<Layer>;

   static auto& get_socket(stream_type& stream) noexcept { return stream.lowest_layer(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
   static bool is_tls(const stream_type&) noexcept { return true; }
};

/// Beast's stream, which wraps a socket to add timeouts and a rate policy.
template <typename Protocol, typename Executor, typename RatePolicy>
struct stream_traits<boost::beast::basic_stream<Protocol, Executor, RatePolicy>>
{
   using stream_type = boost::beast::basic_stream<Protocol, Executor, RatePolicy>;

   static auto& get_socket(stream_type& stream) noexcept { return stream.socket(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
   static bool is_tls(const stream_type&) noexcept { return false; }
};

/// The type-erased stream already offers all three, its implementation has to provide them.
template <>
struct stream_traits<any_async_stream>
{
   using stream_type = any_async_stream;

   static auto& get_socket(stream_type& stream) noexcept { return stream.get_socket(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
   static bool is_tls(const stream_type& stream) noexcept { return stream.is_tls(); }
};

// -------------------------------------------------------------------------------------------------

//
// What the four get_socket() above have in common is TcpSocketBase, declared next to the
// type-erased stream, which returns it directly.
//

/**
 * An async stream that is backed by a TCP socket and knows the executor it runs on -- in other
 * words, something a session can be built on. Note that this deliberately does not match
 * references: the factories taking it move the stream into the session they create.
 */
template <typename Stream>
concept SocketStream = requires(Stream& stream) {
   { stream_traits<Stream>::get_socket(stream) } -> std::convertible_to<TcpSocketBase&>;
   { stream_traits<Stream>::get_executor(stream) } -> std::convertible_to<Executor>;
   { stream_traits<Stream>::is_tls(stream) } -> std::convertible_to<bool>;
};

/**
 * The socket underneath \p stream, for shutdown() and close(). There is no free function for the
 * executor, because the classes calling this have a \c get_executor() of their own, which would
 * hide it.
 */
template <SocketStream Stream>
decltype(auto) get_socket(Stream& stream) noexcept
{
   return stream_traits<Stream>::get_socket(stream);
}

/// Whether \p stream is encrypted, which is what tells "h2" from "h2c".
template <SocketStream Stream>
bool is_tls(const Stream& stream) noexcept
{
   return stream_traits<Stream>::is_tls(stream);
}

// -------------------------------------------------------------------------------------------------

//
// What the sessions do with the connection beneath their stream, apart from reading and writing.
// None of these throws: a connection that is gone already is what the caller wanted anyway, more
// often than not, so the caller decides what an error is worth.
//
namespace io
{

enum class Shutdown
{
   receive,
   send,
   both
};

/// Shuts down one or both directions of the connection beneath \p stream.
template <SocketStream Stream>
error_code shutdown(Stream& stream, Shutdown what) noexcept
{
   using socket_base = boost::asio::socket_base;
   error_code ec;
   get_socket(stream).shutdown(what == Shutdown::receive ? socket_base::shutdown_receive
                               : what == Shutdown::send  ? socket_base::shutdown_send
                                                         : socket_base::shutdown_both,
                               ec);
   return ec;
}

/// Cancels whatever operations are pending on the connection beneath \p stream.
template <SocketStream Stream>
error_code cancel(Stream& stream) noexcept
{
   error_code ec;
   get_socket(stream).cancel(ec);
   return ec;
}

/// Closes the connection beneath \p stream, which also cancels whatever is pending on it.
template <SocketStream Stream>
error_code close(Stream& stream) noexcept
{
   error_code ec;
   get_socket(stream).close(ec);
   return ec;
}

/// The peer of \p stream, if it is connected.
template <SocketStream Stream>
std::optional<boost::asio::ip::tcp::endpoint> remote_endpoint(Stream& stream) noexcept
{
   error_code ec;
   auto endpoint = get_socket(stream).remote_endpoint(ec);
   if (ec)
      return std::nullopt;
   return endpoint;
}

} // namespace io

// -------------------------------------------------------------------------------------------------

/**
 * Ends \p stream as far as the stream itself is concerned, which is something only TLS has: a
 * "close_notify", which tells the peer that the end of the data is the end of the data and not a
 * connection that was cut. Without it, everything the peer reads after the last response fails as
 * a truncated stream instead of ending cleanly. Streams that have nothing of their own to end
 * complete right away, and the FIN that the caller sends afterwards is the whole of it.
 *
 * The peer answers a "close_notify" with one of its own, and one that never does must not keep the
 * session around for good, so the wait for it is bounded: the connection is going away either way.
 */
template <SocketStream Stream>
Task<error_code> async_teardown(Stream& stream)
{
   constexpr auto timeout = std::chrono::seconds(2);

   if constexpr (requires { stream.async_shutdown(boost::asio::as_tuple); })
   {
      auto [ec] = co_await stream.async_shutdown( //
         boost::asio::cancel_after(timeout, boost::asio::as_tuple));
      co_return ec;
   }
   else
      co_return error_code{};
}

// -------------------------------------------------------------------------------------------------

static_assert(SocketStream<boost::asio::ip::tcp::socket>);
static_assert(SocketStream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>);
static_assert(SocketStream<boost::beast::tcp_stream>);
static_assert(SocketStream<any_async_stream>);
static_assert(!SocketStream<boost::asio::ip::tcp::socket&>); // rvalues only, see above

// -------------------------------------------------------------------------------------------------

/**
 * The server's TLS context, from the certificate chain and private key in the PEM files given.
 * ALPN offers "h2" and "http/1.1", and our order of preference wins over the client's.
 */
TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key);

namespace io
{

inline TcpSocket make_socket(const Executor& executor) { return TcpSocket(executor); }
inline TcpAcceptor make_acceptor(const Executor& executor) { return TcpAcceptor(executor); }

/**
 * Opens \p acceptor on \p endpoint and starts listening: with SO_REUSEADDR (and SO_REUSEPORT
 * if \p reuse_port), and on an IPv6
 * endpoint for IPv4 clients, too (dual stack, if the system allows it). Throws what fails.
 */
void listen(TcpAcceptor& acceptor, const asio::ip::tcp::endpoint& endpoint,
            bool reuse_port = false);

inline asio::ip::tcp::endpoint local_endpoint(const TcpAcceptor& acceptor)
{
   return acceptor.local_endpoint();
}

/// Stops accepting: a pending accept() completes with an error.
inline void close(TcpAcceptor& acceptor) noexcept
{
   error_code ec;
   acceptor.close(ec);
}

/// Accepts the next connection into \p socket: <tt>(error_code)</tt>.
inline auto accept(TcpAcceptor& acceptor, TcpSocket& socket)
{
   return acceptor.async_accept(socket, asio::as_tuple);
}

/// Turns off Nagle's algorithm, without which HTTP/2 is very slow (and the TLS handshake slower).
inline void no_delay(TcpSocket& socket)
{
   error_code ec;
   socket.set_option(asio::ip::tcp::no_delay(true), ec);
}

/// The kernel's send and receive buffer sizes of \p socket, for the log.
std::pair<int, int> buffer_sizes(TcpSocket& socket);

/// Resolves \p host and \p port (a number): <tt>(error_code, endpoints)</tt>.
Task<std::tuple<error_code, std::vector<asio::ip::tcp::endpoint>>>
resolve(Executor executor, std::string host, std::string port);

/// Connects \p socket to the first of \p endpoints that takes it: <tt>(error_code, endpoint)</tt>.
Task<std::tuple<error_code, asio::ip::tcp::endpoint>>
connect(TcpSocket& socket, std::vector<asio::ip::tcp::endpoint> endpoints);

inline TlsStream make_tls_stream(TcpSocket&& socket, TlsContext& context)
{
   return TlsStream(std::move(socket), context);
}

/// The TLS handshake, in the role given: <tt>(error_code)</tt>.
inline auto handshake(TlsStream& stream, Role role)
{
   return stream.async_handshake(role == Role::server ? asio::ssl::stream_base::server
                                                      : asio::ssl::stream_base::client,
                                 asio::as_tuple);
}

/// The protocol ALPN has agreed on, empty for none.
std::string_view alpn(TlsStream& stream);

/// The one-line summary of the handshake, see tls_handshake_info().
std::string tls_info(TlsStream& stream);

//
// UDP, for HTTP/3. The backend sends and receives on the native handle, with sendmsg() and
// recvmsg(), and waits for the socket with io::wait_readable() or receives with io::receive().
// What both runtimes do the same way, on the native handle, is in anyhttp/net.hpp.
//

/// A UDP socket, not open yet.
inline UdpSocket make_udp_socket(const Executor& executor) { return UdpSocket(executor); }

/// Opens \p socket for the address family of \p endpoint, non-blocking.
inline error_code open(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept
{
   error_code ec;
   socket.open(endpoint.protocol(), ec);
   if (!ec)
      socket.non_blocking(true, ec);
   return ec;
}

/// Cancels the wait or receive on \p socket: it completes with errc::operation_canceled.
inline void cancel(UdpSocket& socket) noexcept
{
   error_code ec;
   socket.cancel(ec);
}

/// Closes \p socket: a wait or receive on it completes with an error.
inline void close(UdpSocket& socket) noexcept
{
   error_code ec;
   socket.close(ec);
}

} // namespace io

// -------------------------------------------------------------------------------------------------

/**
 * A wake-up that may be sent from any thread, unlike Event. It may also wake up its waiter
 * spuriously, so the waiter checks the condition it waits for itself. With ASIO this is a
 * concurrent channel that holds one wake-up.
 */
class Signal
{
public:
   explicit Signal(const Executor& executor) : channel_(executor, 1) {}

   void notify() { std::ignore = channel_.try_send(error_code{}); }

   /// <tt>auto [ec] = co_await signal.wait();</tt>
   auto wait() { return channel_.async_receive(asio::as_tuple); }

private:
   asio::experimental::concurrent_channel<void(error_code)> channel_;
};

// =================================================================================================

} // namespace anyhttp
