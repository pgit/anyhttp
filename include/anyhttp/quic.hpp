#pragma once

//
// Plain QUIC streams: byte streams of an application protocol of the user's own, identified by
// its ALPN, on the same QUIC transport HTTP/3 runs on. There is no HTTP in them -- no headers, no
// request and response -- only what QUIC itself has: a bidirectional stream with an end in each
// direction (FIN), and two ways of aborting it early (RESET_STREAM, STOP_SENDING).
//
// A Stream is a Reader and a Writer at once, with the contract of both (docs/end-of-body.rst):
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
#include <optional>
#include <string>
#include <vector>

namespace anyhttp::quic
{

// =================================================================================================

/// What plain QUIC connections are made with, on either side.
struct Config
{
   /**
    * The application protocol, as negotiated by ALPN: what the client offers and what the
    * server registers it under. Required, at most 255 bytes.
    */
   std::string alpn;

   /**
    * Client: the CA certificates to verify the server with, see client::Config::tls_ca_file.
    */
   std::string tls_ca_file;

   /**
    * Client: where to log the TLS secrets, see client::Config::tls_keylog_file. The server's is
    * server::Config::tls_keylog_file.
    */
   std::string tls_keylog_file{};

   /**
    * Client: whether the server's certificate is verified at all. Without it, any certificate is
    * taken, from anybody who answers in the server's place -- that is for testing.
    */
   bool tls_verify = true;

   /**
    * Client: how long the connection may be silent. The server's is server::Config::idle_timeout.
    */
   std::chrono::nanoseconds idle_timeout = std::chrono::seconds{30};

   /**
    * Client: the address and port to send from, bound before connecting, with port 0 for any
    * port. Only those of the server's endpoints that are of its family are tried, and if none
    * is, connect() fails with \c errc::address_family_not_supported; an address that is not one
    * of this host's fails with \c errc::address_not_available. Without it, the system picks the
    * address by its routes, and any port.
    */
   std::optional<UdpEndpoint> local_endpoint{};

   /**
    * Client: the name of the network interface to send through, such as \c "eth0", whatever the
    * routes say: SO_BINDTODEVICE, on Linux only, and with \c CAP_NET_RAW before Linux 5.7.
    * connect() fails with \c errc::no_such_device if there is no interface of that name, and with
    * \c errc::operation_not_supported on other systems. Empty sends through whichever interface
    * the routes pick.
    */
   std::string device{};

   /**
    * Flow control, as announced to the peer: how much it may send on one stream, and on all of
    * them together, before we read, and how many streams it may have open at once. A stream's
    * window opens up as its Reader reads, so a stream nobody reads stops its sender at
    * \c max_stream_data.
    */
   uint64_t max_stream_data = 256 * 1024;

   /// How much the peer may send on all streams together, see \ref max_stream_data.
   uint64_t max_data = 1024 * 1024;

   /// How many streams the peer may have open at once, see \ref max_stream_data.
   uint64_t max_streams = 100;

   /**
    * The application error codes for aborts the library makes on the user's behalf: RESET_STREAM
    * when a Writer goes away before write_eof() (or, with \c zero_copy, a write with unacknowledged
    * bytes is cancelled), STOP_SENDING when a Reader goes away before the end. Meaning is up to
    * the protocol.
    */
   uint64_t reset_error_code = 0;

   /// The application error code for STOP_SENDING, see \ref reset_error_code.
   uint64_t stop_sending_error_code = 0;

   /**
    * How writes reach the stack. By default they are copied, in chunks of 16 KiB, and complete
    * once handed over. With \c zero_copy, the stack reads straight from the caller's buffer and a
    * write completes only when the peer has acknowledged all of it -- which saves the copy but
    * costs a round trip per write, and cancelling such a write resets the stream.
    */
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
   /// \cond internal
   Stream(std::shared_ptr<Reader::Impl> reader, std::shared_ptr<Writer::Impl> writer) noexcept;
   Stream(Stream&& other) noexcept;
   Stream& operator=(Stream&& other) noexcept;
   ~Stream();
   /// \endcond

   /// A handle without a stream, as the one that comes with an error.
   Stream() noexcept;

   /// The executor type, for what looks for one.
   using executor_type = Executor;

   /// \return The executor of the connection, or an empty one after \c reset(), with what that
   /// means for an operation started then, see \c Reader::get_executor().
   executor_type get_executor() const noexcept;

   /// \return The <tt>[quic:address:port.stream]</tt> tag the library's log lines carry for this
   /// stream.
   std::string log_prefix() const;

   /// \return Whether either half is still there.
   explicit operator bool() const noexcept
   {
      return Reader::operator bool() || Writer::operator bool();
   }

   /// Releases both halves, as the destructor does.
   void reset() noexcept;

   /// \return The QUIC stream ID, or -1 after \c reset().
   int64_t id() const noexcept;

   /**
    * Aborts the sending side: RESET_STREAM with \p code. A write in progress completes with
    * \c errors::canceled, and every write from then on with \c errc::connection_reset; the peer's
    * reads report \c errors::partial_message.
    *
    * \param code The application error code.
    */
   void reset_stream(uint64_t code);

   /**
    * Asks the peer to stop sending: STOP_SENDING with \p code. What has not been read is dropped,
    * and reads report \c errors::partial_message from then on.
    *
    * \param code The application error code.
    */
   void stop_sending(uint64_t code);

private:
   Reader::Impl* reader_impl() const noexcept;
};

// -------------------------------------------------------------------------------------------------

/// The completion signature of \c Connection::async_accept_stream: the error and the stream.
using AcceptStream = void(error_code, Stream);
using AcceptStreamHandler = Completion<AcceptStream>;

/// The completion signature of \c Connection::async_open_stream: the error and the stream.
using OpenStream = void(error_code, Stream);
using OpenStreamHandler = Completion<OpenStream>;

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
   /// \cond internal
   class Impl;

   explicit Connection(std::shared_ptr<Impl> impl) noexcept;
   Connection(Connection&& other) noexcept;
   Connection& operator=(Connection&& other) noexcept;
   ~Connection();
   /// \endcond

   /// A handle without a connection, as the one that comes with an error.
   Connection() noexcept;

   /// \return Whether this handle has a connection.
   explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

   /// The executor type, for what looks for one.
   using executor_type = Executor;

   /// \return The executor of the connection, which all of it is to be used on, or an empty one
   /// for a handle without a connection, see \c Reader::get_executor().
   executor_type get_executor() const noexcept;

   /// \return The <tt>[quic:address:port]</tt> tag the library's log lines carry for this
   /// connection.
   std::string log_prefix() const;

   /// \return Where the peer sends from and receives at: on the server, the client's address and
   /// port, on the client, the server's. A default-constructed endpoint for a handle without a
   /// connection.
   UdpEndpoint remote_endpoint() const;

#if ANYHTTP_ASIO
   /**
    * Opens a bidirectional stream, waiting until the peer allows another one. The peer learns of
    * the stream with the first thing written.
    *
    * The peer limits how many streams may be open at once, and allows another one as one of
    * them closes: once it is over in both directions, at both ends, which is a round trip after
    * this side is done with it. Until then, this waits, the way a write waits for the peer's flow
    * control window. Opens that wait are served in the order they started.
    *
    * Fails with \c errors::canceled when cancelled, and with \c errors::connection_aborted once
    * the connection is over, also while waiting. A caller that waits here holding the stream that
    * would have to close first waits until then: use \c try_open_stream() to open only if the
    * peer allows it right away.
    *
    * \param token The completion token, for a handler of the signature \c OpenStream.
    * \return What \p token makes of the operation.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(OpenStream) CompletionToken = DefaultCompletionToken>
   auto async_open_stream(CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, OpenStream>(
         boost::asio::bind_executor(boost::asio::get_associated_executor(token, get_executor()),
                                    [this](OpenStreamHandler handler) { //
                                       async_open_stream_any(std::move(handler));
                                    }),
         token);
   }
#endif

   /// \c async_open_stream() as a coroutine: <tt>auto [ec, stream] = co_await
   /// connection.open_stream();</tt>
   /// \return The error and the stream.
   IoTask<Stream> open_stream();

   /**
    * Opens a bidirectional stream if the peer allows another one right now, without waiting. Fails
    * with \c errors::would_block at the peer's limit, and also while an \c open_stream() is
    * waiting, which comes first. On a connection that is over, it fails with
    * \c errors::connection_aborted.
    *
    * \return The error and the stream.
    */
   results<Stream> try_open_stream();

#if ANYHTTP_ASIO
   /// Waits for the next stream the peer opens. Fails with \c errors::connection_aborted once the
   /// connection is over.
   /// \param token The completion token, for a handler of the signature \c AcceptStream.
   /// \return What \p token makes of the operation.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(AcceptStream) CompletionToken = DefaultCompletionToken>
   auto async_accept_stream(CompletionToken&& token = CompletionToken())
   {
      return boost::asio::async_initiate<CompletionToken, AcceptStream>(
         boost::asio::bind_executor(boost::asio::get_associated_executor(token, get_executor()),
                                    [this](AcceptStreamHandler handler) { //
                                       async_accept_stream_any(std::move(handler));
                                    }),
         token);
   }
#endif

   /// \c async_accept_stream() as a coroutine: <tt>auto [ec, stream] = co_await
   /// connection.accept_stream();</tt>
   /// \return The error and the stream.
   IoTask<Stream> accept_stream();

   /// Closes the connection: CONNECTION_CLOSE with the application error \p code. What is still
   /// open fails with \c errc::connection_reset.
   /// \param code The application error code.
   void close(uint64_t code = 0) noexcept;

   /// Releases the connection, closing it, as the destructor does.
   void reset() noexcept;

private:
   void async_open_stream_any(OpenStreamHandler&& handler);
   void async_accept_stream_any(AcceptStreamHandler&& handler);

   std::shared_ptr<Impl> impl_;
};

/// What the server hands each connection to, see Server::on_quic(). The connection is closed
/// when it returns.
using ConnectionHandler = std::function<Task<void>(Connection connection)>;

// -------------------------------------------------------------------------------------------------

/// The completion signature of \c async_connect: the error and the connection.
using Connect = void(error_code, Connection);
using ConnectHandler = Completion<Connect>;

/// Connects to \p host : \p port, negotiating \c config.alpn, and returns the connection.
/// The server's certificate has to be valid for \p host.
/// \param executor What the connection runs on.
/// \param host The name or address of the server.
/// \param port The port of the server.
/// \param config The protocol, and how to speak it.
/// \return The error and the connection.
IoTask<Connection> connect(Executor executor, std::string host, std::string port, Config config);

/// Connects to the first of \p endpoints that gets there, negotiating \c config.alpn, and returns
/// the connection -- for a client that knows the server's address already, and its name apart
/// from that. The server's certificate has to be valid for \p host, which is also sent as SNI if
/// it is a name; an empty \p host checks the chain only, and sends no SNI.
/// \param executor What the connection runs on.
/// \param endpoints Where the server is, tried one after the other.
/// \param host The name or address the server's certificate has to be valid for, or empty.
/// \param config The protocol, and how to speak it.
/// \return The error and the connection.
IoTask<Connection> connect(Executor executor, std::vector<TcpEndpoint> endpoints, std::string host,
                           Config config);

#if ANYHTTP_ASIO
/// \cond internal
void async_connect_any(Executor executor, std::string host, std::string port, Config config,
                       ConnectHandler&& handler);
/// \endcond

/// \c connect() with a completion token.
/// \param executor What the connection runs on.
/// \param host The name or address of the server.
/// \param port The port of the server.
/// \param config The protocol, and how to speak it.
/// \param token The completion token, for a handler of the signature \c Connect.
/// \return What \p token makes of the operation.
template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken = DefaultCompletionToken>
auto async_connect(Executor executor, std::string host, std::string port, Config config,
                   CompletionToken&& token = CompletionToken())
{
   auto handler_executor = boost::asio::get_associated_executor(token, executor);
   return boost::asio::async_initiate<CompletionToken, Connect>(
      boost::asio::bind_executor(
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
