#pragma once

//
// The sessions are templates over the stream they run on, and there are four of those: a plain
// TCP socket, a TLS stream on top of one, beast's tcp_stream and the type-erased any_async_stream.
// Beyond the async read and write operations, which all of them have in common already, a session
// needs two more things from its stream: the underlying socket, to shut it down or close it, and
// an executor to run its loops on. Neither is spelled the same way by all four, so they are
// reached through this trait instead. Ending the stream itself, which only TLS has to do, is
// async and comes as a free function below.
//

#include "anyhttp/detail/any_async_stream.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <chrono>
#include <concepts>

namespace anyhttp
{

// =================================================================================================

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
};

/// A TLS stream, which may be layered on top of anything that has a socket at the bottom.
template <typename Layer>
struct stream_traits<boost::asio::ssl::stream<Layer>>
{
   using stream_type = boost::asio::ssl::stream<Layer>;

   static auto& get_socket(stream_type& stream) noexcept { return stream.lowest_layer(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
};

/// Beast's stream, which wraps a socket to add timeouts and a rate policy.
template <typename Protocol, typename Executor, typename RatePolicy>
struct stream_traits<boost::beast::basic_stream<Protocol, Executor, RatePolicy>>
{
   using stream_type = boost::beast::basic_stream<Protocol, Executor, RatePolicy>;

   static auto& get_socket(stream_type& stream) noexcept { return stream.socket(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
};

/// The type-erased stream already offers both, its implementation has to provide them.
template <>
struct stream_traits<any_async_stream>
{
   using stream_type = any_async_stream;

   static auto& get_socket(stream_type& stream) noexcept { return stream.get_socket(); }
   static auto get_executor(stream_type& stream) noexcept { return stream.get_executor(); }
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
   {
      stream_traits<Stream>::get_executor(stream)
   } -> std::convertible_to<boost::asio::any_io_executor>;
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
boost::asio::awaitable<boost::system::error_code> async_teardown(Stream& stream)
{
   constexpr auto timeout = std::chrono::seconds(2);

   if constexpr (requires { stream.async_shutdown(boost::asio::as_tuple); })
   {
      auto [ec] = co_await stream.async_shutdown( //
         boost::asio::cancel_after(timeout, boost::asio::as_tuple));
      co_return ec;
   }
   else
      co_return boost::system::error_code{};
}

// -------------------------------------------------------------------------------------------------

static_assert(SocketStream<boost::asio::ip::tcp::socket>);
static_assert(SocketStream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>);
static_assert(SocketStream<boost::beast::tcp_stream>);
static_assert(SocketStream<any_async_stream>);
static_assert(!SocketStream<boost::asio::ip::tcp::socket&>); // rvalues only, see above

// =================================================================================================

} // namespace anyhttp
