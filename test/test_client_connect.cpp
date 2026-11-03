#include "test_fixtures.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <format>
#include <stop_token>
#include <string>

using namespace testing;

// =================================================================================================

class ClientConnect : public Test
{
public:
   void SetUp() override { setup_logging(); }

protected:
   /// Connects a client to \p url, on a context of its own, and returns how that went.
   error_code connect(boost::urls::url url)
   {
      client::Client client(context.get_executor(), client::Config{.url = std::move(url)});
      error_code result;
      launch(context.get_executor(), connect(client, result));
      context.run();
      return result;
   }

   static Task<void> connect(client::Client& client, error_code& result)
   {
      auto [ec, session] = co_await client.connect();
      loge("ERROR: {}", ec.message());
      result = ec;
   }

   IoContext context;
};

// -------------------------------------------------------------------------------------------------

TEST_F(ClientConnect, WHEN_unknown_host_THEN_completes_with_host_not_found_eventually)
{
   auto ec = connect(boost::urls::url("http://this-domain-does-not-exist:12345"));
#if ANYHTTP_COROSIO
   // corosio maps the errors of getaddrinfo() onto generic ones
   EXPECT_TRUE(ec == errc::no_such_device_or_address || ec == errc::resource_unavailable_try_again)
      << ec.message();
#else
   EXPECT_TRUE(ec == boost::asio::error::netdb_errors::host_not_found ||
               ec == boost::asio::error::netdb_errors::host_not_found_try_again)
      << ec.message();
#endif
}

TEST_F(ClientConnect, WHEN_wrong_port_THEN_completes_with_connection_refused)
{
   auto port = get_unused_port();
   EXPECT_EQ(connect(boost::urls::url("http://localhost").set_port_number(port)),
             errc::connection_refused);
}

#if ANYHTTP_COROSIO
TEST_F(ClientConnect, WHEN_connect_is_stopped_THEN_returns_operation_canceled)
{
   client::Client client(context.get_executor(),
                         client::Config{.url = boost::urls::url("http://localhost:12345")});
   error_code result;
   std::stop_source stop;
   boost::capy::run_async(context.get_executor(), stop.get_token())(connect(client, result));
   run_later(context.get_executor(), [&] { stop.request_stop(); }); // once it is under way
   context.run();
   EXPECT_EQ(result, errc::operation_canceled);
}
#else
TEST_F(ClientConnect, WHEN_async_connect_is_cancelled_THEN_returns_operation_canceled)
{
   client::Config config{.url = boost::urls::url("http://localhost:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect(cancel_after(0ms, [this](boost::system::error_code ec, Session session) {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
   }));

   context.run();
}
#endif

// -------------------------------------------------------------------------------------------------

namespace
{

/// A UDP socket on loopback that takes whatever arrives and never answers, as a host that does not
/// speak QUIC and drops what it gets. Reading it does not take a running context.
class SilentPeer
{
public:
   SilentPeer() : fd_(::socket(AF_INET, SOCK_DGRAM, 0))
   {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      socklen_t size = sizeof(address);
      EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), size), 0);
      EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size), 0);
      port_ = ntohs(address.sin_port);
   }
   ~SilentPeer() { ::close(fd_); }

   uint16_t port() const noexcept { return port_; }

   /// How many datagrams have arrived so far.
   size_t received()
   {
      std::array<char, 2048> buffer;
      while (::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT) >= 0)
         ++received_;
      return received_;
   }

private:
   int fd_;
   uint16_t port_ = 0;
   size_t received_ = 0;
};

} // namespace

//
// The connect gives up before the peer has answered, and that has to be the end of the connection.
// By then, the handshake has been sent: 100 ms is long after that, and long before the probe that
// would repeat it, and the connection would otherwise go on until its idle timeout of 30 s.
//
TEST_F(ClientConnect, WHEN_http3_connect_is_cancelled_THEN_its_connection_ends_with_it)
{
   SilentPeer peer;
   client::Client client(
      context.get_executor(),
      client::Config{.url = boost::urls::url(std::format("https://127.0.0.1:{}/", peer.port())),
                     .protocol = anyhttp::Protocol::h3});

   error_code result;
   size_t received_on_return = 0;
   auto on_return = [&](error_code ec) {
      result = ec;
      received_on_return = peer.received();
   };

   const auto start = std::chrono::steady_clock::now();
#if ANYHTTP_COROSIO
   auto connect = [](client::Client& client, auto& on_return) -> Task<void> {
      auto [ec, session] = co_await client.connect();
      on_return(ec);
   };
   auto stop_later = [](std::stop_source& stop) -> Task<void> {
      co_await sleep(100ms);
      stop.request_stop();
   };
   std::stop_source stop;
   boost::capy::run_async(context.get_executor(), stop.get_token())(connect(client, on_return));
   launch(context.get_executor(), stop_later(stop));
#else
   client.async_connect(cancel_after(100ms, [&](boost::system::error_code ec, Session) { //
      on_return(ec);
   }));
#endif
   context.run();

   EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
   EXPECT_EQ(result, errc::operation_canceled) << result.message();
   EXPECT_GT(received_on_return, 0u) << "the peer has not even seen the handshake";
   EXPECT_EQ(peer.received(), received_on_return);
}

//
// Nobody listening is what ICMP says, right after the handshake went out. Waiting for the probes
// that repeat it would take 3 s.
//
TEST_F(ClientConnect, WHEN_http3_port_is_closed_THEN_completes_with_connection_refused_at_once)
{
   client::Client client(
      context.get_executor(),
      client::Config{.url =
                        boost::urls::url("https://127.0.0.1").set_port_number(get_unused_port()),
                     .protocol = anyhttp::Protocol::h3});
   error_code result;
   launch(context.get_executor(), connect(client, result));

   const auto start = std::chrono::steady_clock::now();
   context.run();

   EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
   EXPECT_EQ(result, errc::connection_refused) << result.message();
}

TEST_F(ClientConnect, WHEN_connect_to_broadcast_ip_THEN_completes_with_network_unreachable)
{
   EXPECT_EQ(connect(boost::urls::url("http://255.255.255.255:12345")), errc::network_unreachable);
}

// =================================================================================================

//
// A client over TLS verifies the server's certificate, see client::Config::tls_ca_file. Every other
// HTTP/3 test connects successfully with the test PKI's root CA; these make sure that without it,
// or with a host the certificate is not issued for, it doesn't.
//
class ClientTls : public Client
{
protected:
   ClientTls() { url.set_scheme("https"); }

   error_code connect()
   {
      error_code result;
      launch(client->get_executor(), connect(this, result));
      run();
      return result;
   }

   /// HTTP/3 fails with what QUIC closes the connection with, TLS over TCP with what TLS says.
   void expect_failure(const error_code& ec)
   {
      if (GetParam() == anyhttp::Protocol::h3)
         EXPECT_EQ(ec, errc::connection_refused);
      else
         EXPECT_TRUE(ec);
   }

   static Task<void> connect(ClientTls* self, error_code& result)
   {
      auto [ec, session] = co_await self->client->connect();
      result = ec;
      if (session)
         self->protocol = session.protocol();
      self->server.reset();
   }

   std::optional<anyhttp::Protocol> protocol;
};

const auto all_protocols =
   Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2, anyhttp::Protocol::h3);

INSTANTIATE_TEST_SUITE_P(ClientTls, ClientTls, all_protocols, NameGenerator);

TEST_P(ClientTls, WHEN_server_certificate_verifies_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam()); // our server takes what it is offered
}

// -------------------------------------------------------------------------------------------------

class ClientTlsDefaultTrust : public ClientTls
{
protected:
   void configure_client(client::Config& config) override { config.tls_ca_file.clear(); }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsDefaultTrust, ClientTlsDefaultTrust, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsDefaultTrust, WHEN_issuer_is_not_trusted_THEN_connect_fails)
{
   expect_failure(connect());
   EXPECT_FALSE(protocol);
}

// -------------------------------------------------------------------------------------------------

class ClientTlsMissingCaFile : public ClientTls
{
protected:
   void configure_client(client::Config& config) override { config.tls_ca_file = "no/such.pem"; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsMissingCaFile, ClientTlsMissingCaFile, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsMissingCaFile, WHEN_ca_file_cannot_be_loaded_THEN_connect_fails)
{
   EXPECT_TRUE(connect());
   EXPECT_FALSE(protocol);
}

// -------------------------------------------------------------------------------------------------

//
// The test certificate is issued for localhost, anyhttp.invalid, ::1, 127.0.0.1 and 127.0.0.2 --
// not 127.0.0.3.
//
class ClientTlsWrongHost : public ClientTls
{
protected:
   ClientTlsWrongHost() { url = boost::urls::url{"https://127.0.0.3/custom"}; }

   void configure_server(server::Config& config) override { config.listen_address = "127.0.0.3"; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsWrongHost, ClientTlsWrongHost, all_protocols, NameGenerator);

TEST_P(ClientTlsWrongHost, WHEN_certificate_is_for_another_host_THEN_connect_fails)
{
   expect_failure(connect());
   EXPECT_FALSE(protocol);
}

// -------------------------------------------------------------------------------------------------

//
// Cleartext stays what it was: "http" over TCP never touches TLS, so it does not care for a CA file
// it cannot even load.
//
class ClientCleartext : public ClientTlsMissingCaFile
{
protected:
   void configure_client(client::Config& config) override
   {
      ClientTlsMissingCaFile::configure_client(config);
      config.url.set_scheme("http"); // whatever ANYHTTP_TEST_SCHEME says
   }
};

INSTANTIATE_TEST_SUITE_P(ClientCleartext, ClientCleartext,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(ClientCleartext, WHEN_scheme_is_http_THEN_no_tls_is_involved)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam());
}

// =================================================================================================

//
// Connecting where client::Config::endpoints says, for a URL that names no such place: ".invalid"
// never resolves (RFC 6761), so any attempt at resolving it fails the connect, while the test
// certificate is issued for "anyhttp.invalid".
//
class ClientEndpoints : public ClientAsync
{
protected:
   ClientEndpoints() { url = boost::urls::url{"https://anyhttp.invalid/custom"}; }

   void configure_client(client::Config& config) override
   {
      config.endpoints = {server->local_endpoint()};
   }

   /// Responds with the authority the request says it is for.
   void respond_with_authority()
   {
      requestHandler = [](server::Request request, server::Response response) -> Task<void> {
         co_await drain(request);
         const std::string authority(request.url().encoded_host_and_port());
         check(co_await response.submit(200, fields({{"Content-Length", authority.size()}})));
         check(co_await response.write_eof(asio::buffer(authority)));
      };
   }

   Task<void> expect_response_from_server(Session session)
   {
      EXPECT_EQ(session.protocol(), GetParam());
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message.body(), url.encoded_host_and_port());
   }
};

INSTANTIATE_TEST_SUITE_P(ClientEndpoints, ClientEndpoints, all_protocols, NameGenerator);

TEST_P(ClientEndpoints, WHEN_endpoint_is_given_THEN_url_is_verified_and_sent_but_not_resolved)
{
   respond_with_authority();
   clientSession = [this](Session session) -> Task<void> {
      co_await expect_response_from_server(std::move(session));
   };
}

// -------------------------------------------------------------------------------------------------

//
// The first endpoint is nobody's, which TCP learns from a refused connect, and QUIC from an ICMP
// message telling it the port is unreachable.
//
class ClientEndpointsFallback : public ClientEndpoints
{
protected:
   void configure_client(client::Config& config) override
   {
      const auto live = server->local_endpoint();
      config.endpoints = {TcpEndpoint(live.address(), get_unused_port()), live};
   }
};

INSTANTIATE_TEST_SUITE_P(ClientEndpointsFallback, ClientEndpointsFallback, all_protocols,
                         NameGenerator);

TEST_P(ClientEndpointsFallback, WHEN_first_endpoint_fails_THEN_next_one_is_tried)
{
   respond_with_authority();
   clientSession = [this](Session session) -> Task<void> {
      co_await expect_response_from_server(std::move(session));
   };
}

// -------------------------------------------------------------------------------------------------

/// Cleartext has nothing to verify, and the URL still is what requests say they are for.
class ClientEndpointsCleartext : public ClientEndpoints
{
protected:
   void configure_client(client::Config& config) override
   {
      ClientEndpoints::configure_client(config);
      config.url.set_scheme("http"); // whatever ANYHTTP_TEST_SCHEME says
      url.set_scheme("http");
   }
};

INSTANTIATE_TEST_SUITE_P(ClientEndpointsCleartext, ClientEndpointsCleartext,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(ClientEndpointsCleartext, WHEN_endpoint_is_given_THEN_url_is_not_resolved)
{
   respond_with_authority();
   clientSession = [this](Session session) -> Task<void> {
      co_await expect_response_from_server(std::move(session));
   };
}

// -------------------------------------------------------------------------------------------------

/// Being told where to go changes nothing about whom to expect there.
class ClientEndpointsWrongHost : public ClientTls
{
protected:
   ClientEndpointsWrongHost() { url = boost::urls::url{"https://not-in-the-certificate.invalid/"}; }

   void configure_client(client::Config& config) override
   {
      config.endpoints = {server->local_endpoint()};
   }
};

INSTANTIATE_TEST_SUITE_P(ClientEndpointsWrongHost, ClientEndpointsWrongHost, all_protocols,
                         NameGenerator);

TEST_P(ClientEndpointsWrongHost, WHEN_certificate_is_not_for_the_host_of_the_url_THEN_connect_fails)
{
   expect_failure(connect());
   EXPECT_FALSE(protocol);
}

// =================================================================================================
