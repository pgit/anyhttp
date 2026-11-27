#pragma once

//
// The network half of the runtime layer for Boost.Asio, see anyhttp/net.hpp.
//
// The sessions are templates over the stream they run on, and there are three of those: a plain
// TCP socket, a TLS stream on top of one and the type-erased any_async_stream. Beyond the async
// read and write operations, which all of them have in common already, a session needs three more
// things from its stream: the underlying socket, to shut it down or close it, an executor to run
// its loops on, and whether it is encrypted (which tells h2 from h2c). None of them is spelled the
// same way by all three, so they are reached through this trait instead. Ending the
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
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <concepts>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace anyhttp::inline asio
{

// =================================================================================================

using TcpSocket = boost::asio::ip::tcp::socket;
using TcpAcceptor = boost::asio::ip::tcp::acceptor;
using TlsContext = boost::asio::ssl::context;
using TlsStream = boost::asio::ssl::stream<TcpSocket>;
using UdpSocket = boost::asio::ip::udp::socket;

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
#define ANYHTTP_CLIENT_STREAMS(X) X(::anyhttp::TcpSocket) X(::anyhttp::TlsStream)

// -------------------------------------------------------------------------------------------------

//
// I/O on the streams the sessions run on, yielding a tuple in both runtimes:
//
//    auto [ec, n] = co_await io::read_some(stream, buffer);
//
// Always call these qualified: unqualified, ADL would find boost::asio's synchronous read_some()
// and write() for an ASIO stream as well.
//
namespace io
{

/// Reads some bytes into \p buffer: <tt>(error_code, size_t)</tt>.
template <typename Stream>
auto read_some(Stream& stream, boost::asio::mutable_buffer buffer)
{
   return stream.async_read_some(buffer, boost::asio::as_tuple);
}

/// Writes all of \p buffers, unless an error comes first: <tt>(error_code, size_t)</tt>.
template <typename Stream, typename ConstBufferSequence>
auto write(Stream& stream, const ConstBufferSequence& buffers)
{
   //
   // As few writes as the stream takes: async_write()'s default completion condition, transfer_all,
   // would split them into 64 KiB each. A large HTTP/1.1 header then takes several round trips
   // through the reactor, and a server that rejects it early catches the client in the middle.
   //
   return boost::asio::async_write(
      stream, buffers,
      [](const error_code& ec, size_t) -> size_t {
         return ec ? 0 : std::numeric_limits<size_t>::max();
      },
      boost::asio::as_tuple);
}

/// Peeks at what has arrived on \p socket, without taking it: <tt>(error_code, size_t)</tt>.
template <typename Socket>
auto peek(Socket& socket, boost::asio::mutable_buffer buffer)
{
   return socket.async_receive(buffer, Socket::message_peek, boost::asio::as_tuple);
}

/// Receives a datagram into \p buffer, on a connected datagram socket: <tt>(error_code,
/// size_t)</tt>.
template <typename Socket>
auto receive(Socket& socket, boost::asio::mutable_buffer buffer)
{
   return socket.async_receive(buffer, boost::asio::as_tuple);
}

/// Waits until \p socket has something to read, for a caller that reads it by hand
/// (<tt>recvmsg()</tt> on its native handle): <tt>(error_code)</tt>.
template <typename Socket>
auto wait_readable(Socket& socket)
{
   return socket.async_wait(Socket::wait_read, boost::asio::as_tuple);
}

} // namespace io

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
std::optional<TcpEndpoint> remote_endpoint(Stream& stream) noexcept
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
static_assert(SocketStream<any_async_stream>);
static_assert(!SocketStream<boost::asio::ip::tcp::socket&>); // rvalues only, see above

// -------------------------------------------------------------------------------------------------

/**
 * The server's TLS context, from the certificate chain and private key in the PEM files given.
 * ALPN offers \p protocols, and our order of preference wins over the client's, see
 * \c server::Config::tls_protocols; throws what server_alpn() does. The secrets of its handshakes
 * go to \p keylog_file, if not empty, see \c server::Config::tls_keylog_file.
 */
TlsContext make_server_tls_context(const std::string& certificate_chain,
                                   const std::string& private_key,
                                   std::span<const Protocol> protocols,
                                   const std::string& keylog_file = {});

/**
 * A client's TLS context: TLS 1.2 and up, the server's certificate verified against \p ca_file as
 * \c client::Config::tls_ca_file describes -- or not at all, without \p verify. ALPN offers
 * "http/1.1", preceded by "h2" if that is what \p protocol asks for. Secrets go to
 * \p keylog_file, as with the server's. Throws if the CA certificates or the key log cannot be
 * loaded.
 */
TlsContext make_client_tls_context(const std::string& ca_file, Protocol protocol,
                                   const std::string& keylog_file = {}, bool verify = true);

namespace io
{

inline TcpSocket make_socket(const Executor& executor) { return TcpSocket(executor); }
inline TcpAcceptor make_acceptor(const Executor& executor) { return TcpAcceptor(executor); }

/**
 * Opens \p acceptor on \p endpoint and starts listening: with SO_REUSEADDR (and SO_REUSEPORT
 * if \p reuse_port), and on an IPv6 endpoint for IPv6 clients only, or with \p dual_stack for
 * IPv4 clients, too (if the system allows either). Throws what fails.
 */
void listen(TcpAcceptor& acceptor, const TcpEndpoint& endpoint, bool reuse_port = false,
            bool dual_stack = false);

inline TcpEndpoint local_endpoint(const TcpAcceptor& acceptor) { return acceptor.local_endpoint(); }

/// Stops accepting: a pending accept() completes with an error.
inline void close(TcpAcceptor& acceptor) noexcept
{
   error_code ec;
   acceptor.close(ec);
}

/// Accepts the next connection into \p socket: <tt>(error_code)</tt>.
inline auto accept(TcpAcceptor& acceptor, TcpSocket& socket)
{
   return acceptor.async_accept(socket, boost::asio::as_tuple);
}

/// Turns off Nagle's algorithm, without which HTTP/2 is very slow (and the TLS handshake slower).
inline void no_delay(TcpSocket& socket)
{
   error_code ec;
   socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
}

/// The kernel's send and receive buffer sizes of \p socket, for the log.
std::pair<int, int> buffer_sizes(TcpSocket& socket);

/// Pins the send and receive buffer of \p socket, in bytes; either zero leaves that one alone.
/// A size set this way turns the kernel's autotuning off for that direction, which is the point:
/// it bounds how much a write can hand over before it has to wait for the peer. Not worth an
/// error -- the caller logs what it got with buffer_sizes() afterwards.
void set_buffer_sizes(TcpSocket& socket, int send, int receive);

/// Resolves \p host and \p port (a number): <tt>(error_code, endpoints)</tt>.
IoTask<std::vector<TcpEndpoint>> resolve(Executor executor, std::string host, std::string port);

/**
 * Connects \p socket to the first of \p endpoints that takes it: <tt>(error_code, endpoint)</tt>.
 * From \p local, as io::parse_address() has it, and \p port: with either, the socket is bound
 * before each attempt, to io::bind_address() and \p port, with SO_REUSEADDR for a port. Only the
 * endpoints of \p local's family are tried -- if none is, this fails with
 * \c errc::address_family_not_supported. A cancelled attempt is the last one.
 */
IoTask<TcpEndpoint> connect(TcpSocket& socket, std::vector<TcpEndpoint> endpoints,
                            std::optional<IpAddress> local = {}, uint16_t port = 0);

inline TlsStream make_tls_stream(TcpSocket&& socket, TlsContext& context)
{
   return TlsStream(std::move(socket), context);
}

/// Makes a client accept only a certificate valid for \p host, see tls_set_peer_name().
void set_peer_name(TlsStream& stream, const std::string& host);

/// The TLS handshake, in the role given: <tt>(error_code)</tt>.
inline auto handshake(TlsStream& stream, Role role)
{
   return stream.async_handshake(role == Role::server ? boost::asio::ssl::stream_base::server
                                                      : boost::asio::ssl::stream_base::client,
                                 boost::asio::as_tuple);
}

/// The protocol ALPN has agreed on, empty for none.
std::string_view alpn(TlsStream& stream);

/// The one-line summary of the handshake, see tls_handshake_info().
std::string tls_info(TlsStream& stream);

/// Whether a read has failed because the TLS connection ended without a "close_notify".
inline bool is_truncated(const error_code& ec)
{
   return ec == boost::asio::ssl::error::stream_truncated;
}

//
// UDP, for HTTP/3. The backend sends and receives on the native handle, with sendmsg() and
// recvmsg(), and waits for the socket with io::wait_readable() or receives with io::receive().
// What both runtimes do the same way, on the native handle, is in anyhttp/net.hpp.
//

/// A UDP socket, not open yet.
inline UdpSocket make_udp_socket(const Executor& executor) { return UdpSocket(executor); }

/// Opens \p socket for the address family of \p endpoint, non-blocking.
inline error_code open(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept
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
   auto wait() { return channel_.async_receive(boost::asio::as_tuple); }

private:
   boost::asio::experimental::concurrent_channel<void(error_code)> channel_;
};

// =================================================================================================

} // namespace anyhttp::inline asio
