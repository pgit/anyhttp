//
// What the client does over TLS with servers that are not like our own, which speaks TLS 1.3 only,
// always picks HTTP/2 if offered and always ends a connection with a "close_notify". The server
// here is written out by hand, so that it can do all of that differently.
//
#include "test_fixtures.hpp"

#include "anyhttp/net.hpp"

#if ANYHTTP_COROSIO
#include <boost/corosio/tls_context.hpp>
#else
#include <openssl/ssl.h>
#endif

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace testing;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace anyhttp
{

// =================================================================================================

/// The receive window that socket \p fd has advertised last, if the kernel says, see tcp_info.cpp.
std::optional<uint32_t> advertised_receive_window(int fd);

namespace
{

/// What the hand-written server is like.
struct Peer
{
   std::string_view alpn; ///< the one protocol it speaks, empty for not using ALPN at all
   bool tls12 = false; ///< TLS 1.2 is the most it speaks
   std::string_view response; ///< what it responds with, once it has seen a request
   bool close_notify = true; ///< whether it ends the connection with a "close_notify"
};

#if ANYHTTP_COROSIO
TlsContext make_peer_context(const Peer& peer)
{
   const server::Config config;
   TlsContext context;
   EXPECT_FALSE(context.use_certificate_chain_file(config.tls_certificate_chain));
   EXPECT_FALSE(
      context.use_private_key_file(config.tls_private_key, boost::corosio::tls_file_format::pem));
   if (peer.tls12)
      EXPECT_FALSE(context.set_max_protocol_version(boost::corosio::tls_version::tls_1_2));
   if (!peer.alpn.empty())
      EXPECT_FALSE(context.set_alpn({peer.alpn}));
   return context;
}
#else
int select_alpn(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                unsigned int inlen, void* arg)
{
   const auto& wanted = *static_cast<const std::string_view*>(arg);
   for (auto list = std::span{in, inlen}; !list.empty() && list.size() > list[0];
        list = list.subspan(1 + list[0]))
   {
      if (std::string_view{reinterpret_cast<const char*>(&list[1]), list[0]} == wanted)
      {
         *out = &list[1];
         *outlen = list[0];
         return SSL_TLSEXT_ERR_OK;
      }
   }
   return SSL_TLSEXT_ERR_NOACK;
}

TlsContext make_peer_context(const Peer& peer)
{
   const server::Config config;
   boost::asio::ssl::context context{boost::asio::ssl::context::tls_server};
   context.use_certificate_chain_file(config.tls_certificate_chain);
   context.use_private_key_file(config.tls_private_key, boost::asio::ssl::context::pem);
   if (peer.tls12)
      SSL_CTX_set_max_proto_version(context.native_handle(), TLS1_2_VERSION);
   if (!peer.alpn.empty())
      SSL_CTX_set_alpn_select_cb(context.native_handle(), select_alpn,
                                 const_cast<std::string_view*>(&peer.alpn));
   return context;
}
#endif

} // namespace

// =================================================================================================

class ClientTlsPeer : public Test
{
protected:
   void SetUp() override { setup_logging(); }

   /// Runs one GET against the \p peer, which ends the connection afterwards.
   void get(Peer peer)
   {
      auto acceptor = io::make_acceptor(context.get_executor());
      io::listen(acceptor, {io::make_address("127.0.0.2"), 0});
      auto tls = make_peer_context(peer);

      auto url = boost::urls::url("https://127.0.0.2/");
      url.set_port_number(io::local_endpoint(acceptor).port());

      launch(context.get_executor(), serve(acceptor, tls, peer),
             [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

      client::Client client(context.get_executor(),
                            {.url = url, .protocol = protocol, .tls_ca_file = "pki/out/root.pem"});
      launch(context.get_executor(), get(client, url),
             [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

      context.run();
   }

   Task<void> serve(TcpAcceptor& acceptor, TlsContext& tls, Peer peer)
   {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::accept(acceptor, socket));

      auto stream = io::make_tls_stream(std::move(socket), tls);
      check(co_await io::handshake(stream, Role::server));

      std::string head;
      std::array<char, 1024> buffer;
      while (!head.contains("\r\n\r\n"))
      {
         auto [ec, n] = co_await io::read_some(stream, boost::asio::buffer(buffer));
         head.append(buffer.data(), n);
         if (ec)
            throw_error(ec);
      }

      check(co_await io::write(stream, boost::asio::buffer(peer.response)));

      if (peer.close_notify)
         co_await async_teardown(stream);
      io::close(stream);
   }

   Task<void> get(client::Client& client, boost::urls::url url)
   {
      auto session = check(co_await client.connect());
      negotiated = session.protocol();
      auto [ec, message] = co_await session.get(url);
      result = ec;
      body = message.body();
   }

   IoContext context;
   anyhttp::Protocol protocol = anyhttp::Protocol::h2;

   std::optional<anyhttp::Protocol> negotiated;
   error_code result;
   std::string body;
};

constexpr auto ok = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv;
constexpr auto ends_with_the_connection = "HTTP/1.1 200 OK\r\n\r\nhello"sv;

// -------------------------------------------------------------------------------------------------

TEST_F(ClientTlsPeer, WHEN_server_does_not_use_alpn_THEN_session_speaks_http11)
{
   get({.response = ok});
   EXPECT_EQ(negotiated, anyhttp::Protocol::h1);
   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(body, "hello");
}

TEST_F(ClientTlsPeer, WHEN_server_does_not_speak_h2_THEN_session_falls_back_to_http11)
{
   get({.alpn = "http/1.1", .response = ok});
   EXPECT_EQ(negotiated, anyhttp::Protocol::h1);
   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(body, "hello");
}

TEST_F(ClientTlsPeer, WHEN_server_speaks_tls12_only_THEN_connect_succeeds)
{
   get({.alpn = "http/1.1", .tls12 = true, .response = ok});
   EXPECT_EQ(negotiated, anyhttp::Protocol::h1);
   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(body, "hello");
}

// -------------------------------------------------------------------------------------------------

//
// How a body ends with the connection tells it from one that has been cut only over TLS: an
// attacker can send a FIN, but not a "close_notify".
//
TEST_F(ClientTlsPeer, WHEN_body_ends_with_close_notify_THEN_it_is_complete)
{
   get({.response = ends_with_the_connection});
   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(body, "hello");
}

TEST_F(ClientTlsPeer, WHEN_body_ends_without_close_notify_THEN_it_is_truncated)
{
   get({.response = ends_with_the_connection, .close_notify = false});
   EXPECT_EQ(result, errors::partial_message);
}

TEST_F(ClientTlsPeer, WHEN_framed_body_has_arrived_THEN_missing_close_notify_does_not_matter)
{
   get({.response = ok, .close_notify = false});
   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(body, "hello");
}

TEST_F(ClientTlsPeer, WHEN_connection_is_cut_before_the_response_THEN_that_is_the_end_of_stream)
{
   get({.response = "", .close_notify = false});
   EXPECT_EQ(result, error_code{make_error_code(boost::beast::http::error::end_of_stream)});
}

// =================================================================================================

//
// How a connection ends, as seen from a hand-written peer that speaks just enough of the protocol
// of the test: it reads until it cannot any more, and that tells a "close_notify" from a connection
// that was cut.
//
class ClientTlsEnd : public TestWithParam<anyhttp::Protocol>
{
protected:
   void SetUp() override { setup_logging(); }

   bool h2() const { return GetParam() == anyhttp::Protocol::h2; }

   /// What the peer does after the client has spoken, instead of reading on.
   enum class Then
   {
      read_on,
      end_the_session, ///< sends a GOAWAY, and waits for the client's "close_notify"
      stall ///< stops reading for a while, then closes
   };

   /// Runs \p client_side against the peer, until that has seen the end of the connection.
   template <typename F>
   void run(F client_side, Then then = Then::read_on)
   {
      auto acceptor = io::make_acceptor(context.get_executor());
      io::listen(acceptor, {io::make_address("127.0.0.2"), 0});
      const Peer options{.alpn =
                            h2() ? "h2" : "http/1.1"}; // the context keeps pointing to its ALPN
      auto tls = make_peer_context(options);

      auto url = boost::urls::url("https://127.0.0.2/");
      url.set_port_number(io::local_endpoint(acceptor).port());

      launch(context.get_executor(), peer(acceptor, tls, then),
             [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

      target = url;
      client::Client client(context.get_executor(), {.url = url,
                                                     .protocol = GetParam(),
                                                     .socket_send_buffer_size = 64 * 1024,
                                                     .tls_ca_file = "pki/out/root.pem"});
      launch(context.get_executor(), client_side(client),
             [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

      context.run();
   }

   Task<void> peer(TcpAcceptor& acceptor, TlsContext& tls, Then then)
   {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::accept(acceptor, socket));

      auto stream = io::make_tls_stream(std::move(socket), tls);
      check(co_await io::handshake(stream, Role::server));

      std::array<char, 16384> buffer;
      for (bool first = true;; first = false)
      {
         auto [ec, n] = co_await io::read_some(stream, boost::asio::buffer(buffer));
         received.append(buffer.data(), n);
         if (ec)
         {
            end = ec;
            break;
         }

         //
         // Not before the client has spoken: a client that lets go of the session right away would
         // make this fail. Neither does it matter if it does.
         //
         if (first && h2())
            co_await io::write(stream, boost::asio::buffer(then == Then::end_the_session
                                                              ? settings_and_goaway
                                                           : then == Then::stall ? big_windows
                                                                                 : settings));

         if (then == Then::end_the_session)
         {
            end = co_await async_teardown(stream);
            break;
         }

         //
         // HTTP/2 has to get to the end of its wait for the GOAWAY to be written first, see
         // ClientSession::shutdown().
         //
         if (then == Then::stall)
         {
            co_await until_full(stream);
            stalled.set();
            co_await delay(h2() ? 2200ms : 200ms);
            break;
         }
      }

      if (end == errors::eof) // it is our turn to answer that
         co_await async_teardown(stream);
      io::close(stream);
      peer_done.set();
   }

   ///
   /// Returns once \p stream has closed its receive window to the client, on two samples in a row.
   /// The window stays closed, as nothing is read any more, and the second sample gives the client
   /// the time to fill its send buffer behind it: then its write is stuck for good.
   ///
   /// A fixed time is not enough for that: the client may get to run so little that its write is
   /// still under way when shutdown() is called, and the GOAWAY fits in after it. Neither is a
   /// receive queue that has stopped growing: it does so for tens of milliseconds at a time.
   ///
   Task<void> until_full(TlsStream& stream)
   {
      const auto fd = get_socket(stream).native_handle();
      for (int closed = 0; closed < 2;)
      {
         co_await delay(10ms);
         auto window = advertised_receive_window(fd);
         if (!window)
         {
            ADD_FAILURE() << "the kernel does not report the receive window";
            break;
         }
         closed = *window == 0 ? closed + 1 : 0;
      }
   }

   /// Whether a frame of \p type has arrived, of the HTTP/2 the peer has received.
   bool received_frame(uint8_t type) const
   {
      constexpr size_t preface_size = 24, header_size = 9;
      for (size_t pos = preface_size; pos + header_size <= received.size();)
      {
         const auto* header = reinterpret_cast<const uint8_t*>(received.data() + pos);
         if (header[3] == type)
            return true;
         pos += header_size + ((header[0] << 16) | (header[1] << 8) | header[2]);
      }
      return false;
   }

   static constexpr auto settings = "\0\0\0\x04\0\0\0\0\0"sv;
   static constexpr auto settings_and_goaway =
      "\0\0\0\x04\0\0\0\0\0\0\0\x08\x07\0\0\0\0\0\0\0\0\0\0\0\0\0"sv;
   static constexpr uint8_t goaway_frame = 7;

   /// SETTINGS_INITIAL_WINDOW_SIZE and a WINDOW_UPDATE of the connection, to the maximum of each:
   /// without flow control in the way, what stops HTTP/2 from writing is the socket.
   static constexpr auto big_windows = "\0\0\x06\x04\0\0\0\0\0\0\x04\x7f\xff\xff\xff"
                                       "\0\0\x04\x08\0\0\0\0\0\x7f\xff\0\0"sv;

   IoContext context;
   boost::urls::url target;
   std::string received;
   error_code end;
   Event stalled; ///< set once the client's write is stuck, with Then::stall
   Event peer_done;
};

INSTANTIATE_TEST_SUITE_P(ClientTlsEnd, ClientTlsEnd,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(ClientTlsEnd, WHEN_session_is_shut_down_THEN_peer_sees_a_clean_end)
{
   error_code result = errors::canceled;
   run([&](client::Client& client) -> Task<void> {
      auto session = check(co_await client.connect());
      std::tie(result) = co_await session.shutdown();
   });

   EXPECT_EQ(result, error_code{});
   EXPECT_EQ(end, errors::eof);
   if (h2())
      EXPECT_TRUE(received_frame(goaway_frame));
}

TEST_P(ClientTlsEnd, WHEN_session_is_destroyed_THEN_peer_sees_a_cut_connection)
{
   run([&](client::Client& client) -> Task<void> {
      auto session = check(co_await client.connect());
      session.reset();
   });

   // no "close_notify", and whether it is a FIN or an RST depends on what the client left unread
   EXPECT_TRUE(end);
   EXPECT_NE(end, errors::eof);
}

TEST_P(ClientTlsEnd, WHEN_shutdown_is_repeated_THEN_nothing_happens)
{
   run([&](client::Client& client) -> Task<void> {
      auto session = check(co_await client.connect());
      auto [first] = co_await session.shutdown();
      EXPECT_EQ(first, error_code{});
      auto [second] = co_await session.shutdown();
      EXPECT_EQ(second, error_code{});
   });

   EXPECT_EQ(end, errors::eof);
}

//
// The peer waits for the client's "close_notify". A client that did not answer it before the
// session goes away would keep it waiting until it gives up. What it gets to see depends on the TLS
// library when the client has anything else to say first, so only that it has not given up is
// checked here.
//
TEST_P(ClientTlsEnd, WHEN_peer_ends_the_session_THEN_client_answers_without_being_asked)
{
   if (!h2())
      GTEST_SKIP() << "an HTTP/1.1 client session has nothing running to notice that";

   run(
      [&](client::Client& client) -> Task<void> {
         auto session = check(co_await client.connect());
         co_await peer_done.wait();
      },
      Then::end_the_session);

   EXPECT_NE(end, errc::operation_canceled);
}

//
// A body far larger than the socket buffers, to a peer that has stopped reading. The write is
// interrupted somewhere in a TLS record, for which a "close_notify" would come too late.
//
TEST_P(ClientTlsEnd, WHEN_write_is_stuck_THEN_shutdown_interrupts_it_and_says_so)
{
   error_code write_result, result;
   const std::vector<char> body(32 * 1024 * 1024);
   run(
      [&](client::Client& client) -> Task<void> {
         auto session = check(co_await client.connect());
         auto request = check(co_await session.submit(target, {}));

         auto write = [&]() -> Task<void> {
            std::tie(write_result) = co_await request.write(boost::asio::buffer(body));
         };
         auto end_it = [&]() -> Task<void> {
            co_await stalled.wait();
            std::tie(result) = co_await session.shutdown();
         };
         co_await when_both(write(), end_it());
      },
      Then::stall);

   EXPECT_TRUE(write_result);
   EXPECT_EQ(result, errors::connection_aborted);
}

// =================================================================================================

//
// What happens to what is in progress, against our own server, and with HTTP/3 as well.
//
class ClientShutdown : public ClientAsync
{
protected:
   ClientShutdown() { url.set_scheme("https"); }
};

INSTANTIATE_TEST_SUITE_P(ClientShutdown, ClientShutdown,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

TEST_P(ClientShutdown, WHEN_session_is_shut_down_THEN_it_speaks_the_protocol_it_was_asked_to)
{
   clientSession = [this](Session session) -> Task<void> {
      EXPECT_EQ(session.protocol(), GetParam());
      auto [ec] = co_await session.shutdown();
      EXPECT_EQ(ec, error_code{});
   };
}

TEST_P(ClientShutdown, WHEN_session_is_shut_down_THEN_new_requests_fail)
{
   clientSession = [this](Session session) -> Task<void> {
      auto [ec] = co_await session.shutdown();
      EXPECT_EQ(ec, error_code{});
      auto [submit_ec, request] = co_await session.submit(url.set_path("echo"), {});
      EXPECT_TRUE(submit_ec);
   };
}

TEST_P(ClientShutdown, WHEN_request_is_open_THEN_it_fails_right_away)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      auto read = [&]() -> Task<void> {
         std::array<uint8_t, 1024> buffer;
         auto [ec, n] = co_await response.read_some(boost::asio::buffer(buffer));
         EXPECT_TRUE(ec);
         EXPECT_NE(ec, errors::eof);
         EXPECT_EQ(n, 0);
      };
      auto end = [&]() -> Task<void> {
         co_await yield(10);
         auto [ec] = co_await session.shutdown();
         EXPECT_EQ(ec, error_code{});
      };
      co_await when_both(read(), end());

      auto [write_ec] = co_await request.write(boost::asio::const_buffer("x", 1));
      EXPECT_TRUE(write_ec);
   };
}

// -------------------------------------------------------------------------------------------------

class ClientShutdownCleartext : public ClientAsync
{
protected:
   void configure_client(client::Config& config) override { config.url.set_scheme("http"); }
};

INSTANTIATE_TEST_SUITE_P(ClientShutdownCleartext, ClientShutdownCleartext,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(ClientShutdownCleartext, WHEN_session_is_shut_down_THEN_there_is_just_nothing_to_notify)
{
   clientSession = [](Session session) -> Task<void> {
      auto [ec] = co_await session.shutdown();
      EXPECT_EQ(ec, error_code{});
   };
}

// =================================================================================================

} // namespace anyhttp
