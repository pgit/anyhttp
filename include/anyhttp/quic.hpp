#pragma once

//
// Plain QUIC streams: byte streams of an application protocol of the user's own, identified by
// its ALPN, on the same QUIC transport HTTP/3 runs on. There is no HTTP in them -- no headers, no
// request and response -- only what QUIC itself has: a bidirectional stream with an end in each
// direction (FIN), and two ways of aborting it early (RESET_STREAM, STOP_SENDING).
//
// A Stream is a Reader and a Writer at once, with the contract of both (README.md, "The End of a
// Body"):
//
//    QUIC           Writer side            Reader side
//    FIN            write_eof()            errors::eof
//    RESET_STREAM   reset_stream()         errors::partial_message
//    STOP_SENDING   writes fail with       stop_sending()
//                   connection_reset
//
// The client connects with quic::connect(), the server registers the protocol with
// Server::on_quic() and gets every connection that negotiates it handed to its handler, on the
// same UDP port as HTTP/3.
//

#include "common.hpp"
#include "reader.hpp"
#include "writer.hpp"

#if ANYHTTP_ASIO
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#endif

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace anyhttp::quic
{

// =================================================================================================

/// What plain QUIC connections are made with, on either side.
struct Config
{
   /// The application protocol, as negotiated by ALPN: what the client offers and what the
   /// server registers it under. Required, at most 255 bytes.
   std::string alpn;

   /// Client: the CA certificates to verify the server with, see client::Config::tls_ca_file.
   std::string tls_ca_file;

   /// Client: how long the connection may be silent. The server's is server::Config::idle_timeout.
   std::chrono::nanoseconds idle_timeout = std::chrono::seconds{30};

   //
   // Flow control, as announced to the peer: how much it may send on one stream, and on all of
   // them together, before we read, and how many streams it may have open at once. A stream's
   // window opens up as its Reader reads, so a stream nobody reads stops its sender at
   // `max_stream_data`.
   //
   uint64_t max_stream_data = 256 * 1024;
   uint64_t max_data = 1024 * 1024;
   uint64_t max_streams = 100;

   //
   // The application error codes for aborts the library makes on the user's behalf: RESET_STREAM
   // when a Writer goes away before write_eof() (or, with `zero_copy`, a write with unacknowledged
   // bytes is cancelled), STOP_SENDING when a Reader goes away before the end. Meaning is up to
   // the protocol.
   //
   uint64_t reset_error_code = 0;
   uint64_t stop_sending_error_code = 0;

   //
   // How writes reach the stack. By default they are copied, in chunks of 16 KiB, and complete
   // once handed over. With `zero_copy`, the stack reads straight from the caller's buffer and a
   // write completes only when the peer has acknowledged all of it -- which saves the copy but
   // costs a round trip per write, and cancelling such a write resets the stream.
   //
   bool zero_copy = false;
};

// =================================================================================================

/**
 * A bidirectional QUIC stream: a \c Reader for what the peer sends and a \c Writer for what we
 * send, independent of each other.
 *
 * The two halves can be used concurrently (one read and one write at a time), and handed off on
 * their own by moving them into a \c Reader or \c Writer -- dropping one half ends just that
 * direction. Dropping a Writer before \c write_eof() resets the stream's sending side, dropping a
 * Reader before \c errors::eof stops the peer's.
 */
class Stream : public Reader, public Writer
{
public:
   Stream() noexcept;
   Stream(std::shared_ptr<Reader::Impl> reader, std::shared_ptr<Writer::Impl> writer) noexcept;
   Stream(Stream&& other) noexcept;
   Stream& operator=(Stream&& other) noexcept;
   ~Stream();

   using executor_type = Executor;

   /// The executor of the connection, or an empty one after \c reset().
   executor_type get_executor() const noexcept;

   /// The \c [quic:address:port.stream] tag the library's log lines carry for this stream.
   std::string log_prefix() const;

   /// Whether either half is still there.
   explicit operator bool() const noexcept
   {
      return Reader::operator bool() || Writer::operator bool();
   }

   /// Releases both halves, as the destructor does.
   void reset() noexcept;

   /// The QUIC stream ID, or -1 after \c reset().
   int64_t id() const noexcept;

   /**
    * Aborts the sending side: RESET_STREAM with \p code. A write in progress completes with
    * \c errors::canceled, and every write from then on with \c errc::connection_reset; the peer's
    * reads report \c errors::partial_message.
    */
   void reset_stream(uint64_t code);

   /**
    * Asks the peer to stop sending: STOP_SENDING with \p code. What has not been read is dropped,
    * and reads report \c errors::partial_message from then on.
    */
   void stop_sending(uint64_t code);

private:
   Reader::Impl* reader_impl() const noexcept;
};

// -------------------------------------------------------------------------------------------------

using AcceptStream = void(error_code, Stream);
using AcceptStreamHandler = Completion<AcceptStream>;

/**
 * One QUIC connection carrying plain streams, as a handle. Dropping it (or \c reset()) closes the
 * connection, failing whatever streams are still open, as \c close() does.
 *
 * All of it is to be used on the connection's executor: on the server, that is where the
 * connection handler runs, on the client, the executor passed to \c connect().
 */
class Connection
{
public:
   class Impl;

   Connection() noexcept;
   explicit Connection(std::shared_ptr<Impl> impl) noexcept;
   Connection(Connection&& other) noexcept;
   Connection& operator=(Connection&& other) noexcept;
   ~Connection();

   explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

   using executor_type = Executor;
   executor_type get_executor() const noexcept;

   /// The \c [quic:address:port] tag the library's log lines carry for this connection.
   std::string log_prefix() const;

   /**
    * Opens a bidirectional stream. The peer learns of it with the first thing written.
    *
    * Opening a stream never waits: at the limit of concurrent streams the peer allows, this fails
    * with \c errors::would_block -- the caller may be the one who has to finish a stream first.
    * On a connection that is over, it fails with \c errors::connection_aborted.
    */
   std::tuple<error_code, Stream> open_stream();

#if ANYHTTP_ASIO
   /// Waits for the next stream the peer opens. Fails with \c errors::connection_aborted once the
   /// connection is over.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(AcceptStream) CompletionToken = DefaultCompletionToken>
   auto async_accept_stream(CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, AcceptStream>(
         asio::bind_executor(asio::get_associated_executor(token, get_executor()),
                             [this](AcceptStreamHandler handler) { //
                                async_accept_stream_any(std::move(handler));
                             }),
         token);
   }
#endif

   /// \c async_accept_stream() as a coroutine: <tt>auto [ec, stream] = co_await
   /// connection.accept_stream();</tt>
   Task<std::tuple<error_code, Stream>> accept_stream();

   /// Closes the connection: CONNECTION_CLOSE with the application error \p code. What is still
   /// open fails with \c errc::connection_reset.
   void close(uint64_t code = 0) noexcept;

   /// Releases the connection, closing it, as the destructor does.
   void reset() noexcept;

private:
   void async_accept_stream_any(AcceptStreamHandler&& handler);

   std::shared_ptr<Impl> impl_;
};

/// What the server hands each connection to, see Server::on_quic(). The connection is closed
/// when it returns.
using ConnectionHandler = std::function<Task<void>(Connection connection)>;

// -------------------------------------------------------------------------------------------------

using Connect = void(error_code, Connection);
using ConnectHandler = Completion<Connect>;

/// Connects to \p host : \p port, negotiating \c config.alpn, and returns the connection.
/// The server's certificate has to be valid for \p host.
Task<std::tuple<error_code, Connection>> connect(Executor executor, std::string host,
                                                 std::string port, Config config);

#if ANYHTTP_ASIO
void async_connect_any(Executor executor, std::string host, std::string port, Config config,
                       ConnectHandler&& handler);

/// \c connect() with a completion token.
template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken = DefaultCompletionToken>
auto async_connect(Executor executor, std::string host, std::string port, Config config,
                   CompletionToken&& token = CompletionToken())
{
   auto handler_executor = asio::get_associated_executor(token, executor);
   return asio::async_initiate<CompletionToken, Connect>(
      asio::bind_executor(
         handler_executor,
         [executor = std::move(executor), host = std::move(host), port = std::move(port),
          config = std::move(config)](ConnectHandler handler) mutable {
            async_connect_any(std::move(executor), std::move(host), std::move(port),
                              std::move(config), std::move(handler));
         }),
      token);
}
#endif

// =================================================================================================

} // namespace anyhttp::quic
