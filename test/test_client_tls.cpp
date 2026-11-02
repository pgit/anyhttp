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
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace testing;
using namespace std::string_view_literals;

namespace anyhttp
{

// =================================================================================================

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
   namespace corosio = boost::corosio;
   const server::Config config;
   TlsContext context;
   EXPECT_FALSE(context.use_certificate_chain_file(config.tls_certificate_chain));
   EXPECT_FALSE(
      context.use_private_key_file(config.tls_private_key, corosio::tls_file_format::pem));
   if (peer.tls12)
      EXPECT_FALSE(context.set_max_protocol_version(corosio::tls_version::tls_1_2));
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
   asio::ssl::context context{asio::ssl::context::tls_server};
   context.use_certificate_chain_file(config.tls_certificate_chain);
   context.use_private_key_file(config.tls_private_key, asio::ssl::context::pem);
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
         auto [ec, n] = co_await io::read_some(stream, asio::buffer(buffer));
         head.append(buffer.data(), n);
         if (ec)
            throw_error(ec);
      }

      check(co_await io::write(stream, asio::buffer(peer.response)));

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

} // namespace anyhttp
