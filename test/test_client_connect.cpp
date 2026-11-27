#include "test_fixtures.hpp"

#include "anyhttp/net.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
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

//
// What the server offers over TLS on TCP, see server::Config::tls_protocols. HTTP/3 is QUIC's to
// negotiate, so a client that asks for it gets it whatever the list says.
//
class ClientTlsServerHttp11Only : public ClientTls
{
protected:
   void configure_server(server::Config& config) override { config.tls_protocols = {Protocol::h1}; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsServerHttp11Only, ClientTlsServerHttp11Only, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsServerHttp11Only, WHEN_server_offers_http11_only_THEN_client_speaks_http11)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam() == Protocol::h3 ? Protocol::h3 : Protocol::h1);
}

class ClientTlsServerPrefersHttp11 : public ClientTls
{
protected:
   void configure_server(server::Config& config) override
   {
      config.tls_protocols = {Protocol::h1, Protocol::h2};
   }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsServerPrefersHttp11, ClientTlsServerPrefersHttp11, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsServerPrefersHttp11, WHEN_server_prefers_http11_THEN_client_speaks_http11)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam() == Protocol::h3 ? Protocol::h3 : Protocol::h1);
}

//
// A client that offers only HTTP/1.1 agrees on nothing with this server, and is refused, see
// server::Config::tls_protocols.
//
class ClientTlsServerHttp2Only : public ClientTls
{
protected:
   void configure_server(server::Config& config) override { config.tls_protocols = {Protocol::h2}; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsServerHttp2Only, ClientTlsServerHttp2Only, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsServerHttp2Only, WHEN_server_offers_http2_only_THEN_http11_client_is_refused)
{
   if (GetParam() == Protocol::h1)
   {
      expect_failure(connect());
      EXPECT_FALSE(protocol);
   }
   else
   {
      EXPECT_EQ(connect(), error_code{});
      EXPECT_EQ(protocol, GetParam());
   }
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
// Both ends log the secrets of their handshakes, see server::Config::tls_keylog_file, each to a
// file of its own here. They have done the same handshake, so they have to log the same secrets.
//
// Once the handshake is *done*, that is: a QUIC client is connected as soon as it has its own
// keys, before the server has seen its Finished, which is when a server on OpenSSL logs the
// client's traffic secret. A response is proof that it has: the server reads the request with it.
//
class ClientTlsKeyLog : public ClientTls
{
protected:
   error_code get()
   {
      error_code result;
      launch(client->get_executor(), get(this, result));
      run();
      return result;
   }

   static Task<void> get(ClientTlsKeyLog* self, error_code& result)
   {
      auto [ec, session] = co_await self->client->connect();
      if (!ec)
      {
         auto url = self->url;
         url.set_path("/echo");
         std::tie(ec, std::ignore) = co_await session.get(std::move(url));
      }
      result = ec;
      self->server.reset();
   }

   ~ClientTlsKeyLog() override
   {
      std::filesystem::remove(server_file);
      std::filesystem::remove(client_file);
   }

   void configure_server(server::Config& config) override
   {
      config.tls_keylog_file = server_file.string();
   }

   void configure_client(client::Config& config) override
   {
      config.tls_keylog_file = client_file.string();
   }

   /// The lines of \p file, sorted, or none if there is no such file.
   static std::multiset<std::string> lines(const std::filesystem::path& file)
   {
      std::multiset<std::string> result;
      std::ifstream in(file);
      for (std::string line; std::getline(in, line);)
         result.insert(line);
      return result;
   }

   static std::filesystem::path path(std::string_view role)
   {
      const auto* test = UnitTest::GetInstance()->current_test_info();
      auto name = std::format("anyhttp-keylog-{}-{}-{}.txt", ::getpid(), test->name(), role);
      std::ranges::replace(name, '/', '-');
      return std::filesystem::temp_directory_path() / name;
   }

   const std::filesystem::path server_file = path("server");
   const std::filesystem::path client_file = path("client");
};

INSTANTIATE_TEST_SUITE_P(ClientTlsKeyLog, ClientTlsKeyLog, all_protocols, NameGenerator);

TEST_P(ClientTlsKeyLog, WHEN_keylog_file_is_set_THEN_both_ends_log_the_same_secrets)
{
   ASSERT_EQ(get(), error_code{});

#if ANYHTTP_COROSIO
   if (GetParam() != anyhttp::Protocol::h3)
   {
      // corosio's TLS stream has no keylog; the server's QUIC context opened the file all the same
      EXPECT_THAT(lines(client_file), IsEmpty());
      EXPECT_THAT(lines(server_file), IsEmpty());
      return;
   }
#endif

   const auto logged = lines(client_file);
   EXPECT_EQ(logged, lines(server_file));

   //
   // TLS 1.3, which our server insists on, in the NSS key log format: a label, the client random
   // (32 bytes) and the secret, both in hex.
   //
   std::set<std::string> labels;
   for (const auto& line : logged)
   {
      std::istringstream fields(line);
      std::string label, client_random, secret;
      fields >> label >> client_random >> secret;
      EXPECT_EQ(client_random.size(), 64u) << line;
      EXPECT_FALSE(secret.empty()) << line;
      labels.insert(label);
   }
   EXPECT_THAT(labels,
               IsSupersetOf({"CLIENT_HANDSHAKE_TRAFFIC_SECRET", "SERVER_HANDSHAKE_TRAFFIC_SECRET",
                             "CLIENT_TRAFFIC_SECRET_0", "SERVER_TRAFFIC_SECRET_0"}));
}

// -------------------------------------------------------------------------------------------------

//
// A key log that can't be opened fails the connect, as a CA file that can't be loaded does: the
// caller asked for the secrets, and would not otherwise notice that they are not there.
//
class ClientTlsBadKeyLog : public ClientTls
{
protected:
   void configure_client(client::Config& config) override
   {
      config.tls_keylog_file = "no/such/directory/keylog.txt";
   }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsBadKeyLog, ClientTlsBadKeyLog,
                         Values(anyhttp::Protocol::h3
#if ANYHTTP_ASIO
                                ,
                                anyhttp::Protocol::h1, anyhttp::Protocol::h2
#endif
                                ),
                         NameGenerator);

TEST_P(ClientTlsBadKeyLog, WHEN_keylog_file_cannot_be_opened_THEN_connect_fails)
{
   EXPECT_TRUE(connect());
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
         check(co_await response.write_eof(make_buffer(authority)));
      };
   }

   Task<void> expect_response_from_server(Session session)
   {
      EXPECT_EQ(session.protocol(), GetParam());
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.status_int(), 200);
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

//
// client::Config::tls_verify: without it, a certificate is taken that the trust store does not
// vouch for, and one that is for another host.
//
class ClientTlsNoVerify : public ClientTlsDefaultTrust
{
protected:
   void configure_client(client::Config& config) override
   {
      ClientTlsDefaultTrust::configure_client(config);
      config.tls_verify = false;
   }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsNoVerify, ClientTlsNoVerify, all_protocols, NameGenerator);

TEST_P(ClientTlsNoVerify, WHEN_issuer_is_not_trusted_but_verify_is_off_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam());
}

class ClientTlsNoVerifyWrongHost : public ClientTlsWrongHost
{
protected:
   void configure_client(client::Config& config) override { config.tls_verify = false; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsNoVerifyWrongHost, ClientTlsNoVerifyWrongHost, all_protocols,
                         NameGenerator);

TEST_P(ClientTlsNoVerifyWrongHost, WHEN_certificate_is_for_another_host_but_verify_is_off_THEN_ok)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam());
}

// =================================================================================================

//
// client::Config::local_address and local_port, which the server's listener sees a TCP connection
// come from. 127.0.0.3 is on loopback, too, on Linux.
//
class ClientLocalAddress : public ClientConnect
{
protected:
   /// Connects to a listener on 127.0.0.2 from \p address and \p port, and returns where the
   /// listener saw the connection come from, if it did.
   std::optional<TcpEndpoint> connect_from(std::string address, uint16_t port = 0)
   {
      auto acceptor = io::make_acceptor(context.get_executor());
      io::listen(acceptor, {io::make_address("127.0.0.2"), 0});
      const auto server = io::local_endpoint(acceptor);

      std::optional<TcpEndpoint> seen;
      auto accept = [this](TcpAcceptor& acceptor, std::optional<TcpEndpoint>& seen) -> Task<void> {
         auto socket = io::make_socket(context.get_executor());
         check(co_await io::accept(acceptor, socket));
         seen = io::remote_endpoint(socket);
      };
      launch(context.get_executor(), accept(acceptor, seen),
             [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

      auto url = boost::urls::url("http://127.0.0.2/");
      url.set_port_number(server.port());
      client::Client client(context.get_executor(), {.url = url,
                                                     .local_address = std::move(address),
                                                     .local_port = port,
                                                     .protocol = Protocol::h1});
      auto connect_or_close = [](client::Client& client, error_code& result,
                                 TcpAcceptor& acceptor) -> Task<void> {
         co_await connect(client, result);
         if (result)
            io::close(acceptor); // or its accept() would wait for ever
      };
      launch(context.get_executor(), connect_or_close(client, result, acceptor));
      context.run();
      return seen;
   }

   /// A free port, the kernel's pick for a listener: closing that leaves nothing behind.
   uint16_t free_port()
   {
      auto probe = io::make_acceptor(context.get_executor());
      io::listen(probe, {io::make_address("127.0.0.3"), 0});
      const auto port = io::local_endpoint(probe).port();
      io::close(probe);
      return port;
   }

   error_code result;
};

TEST_F(ClientLocalAddress, WHEN_local_address_is_given_THEN_connection_comes_from_there)
{
   const auto seen = connect_from("127.0.0.3");
   EXPECT_EQ(result, error_code{}) << result.message();
   ASSERT_TRUE(seen);
   EXPECT_EQ(seen->address(), io::make_address("127.0.0.3"));
}

TEST_F(ClientLocalAddress, WHEN_local_address_is_ipv4_mapped_THEN_it_is_the_ipv4_address)
{
   const auto seen = connect_from("::ffff:127.0.0.3");
   EXPECT_EQ(result, error_code{}) << result.message();
   ASSERT_TRUE(seen);
   EXPECT_EQ(seen->address(), io::make_address("127.0.0.3"));
}

// "*" is either family, so the port alone is bound, and to the server's family.
TEST_F(ClientLocalAddress, WHEN_any_address_of_either_family_and_a_port_THEN_that_port_is_used)
{
   const auto port = free_port();
   const auto seen = connect_from("*", port);
   EXPECT_EQ(result, error_code{}) << result.message();
   ASSERT_TRUE(seen);
   EXPECT_EQ(seen->port(), port);
}

TEST_F(ClientLocalAddress, WHEN_any_ipv4_address_and_a_port_THEN_that_port_is_used)
{
   const auto port = free_port();
   const auto seen = connect_from("0.0.0.0", port);
   EXPECT_EQ(result, error_code{}) << result.message();
   ASSERT_TRUE(seen);
   EXPECT_EQ(seen->port(), port);
}

//
// With a port: the first connection from it is still around, in FIN_WAIT or TIME_WAIT, when the
// second binds it again, which takes SO_REUSEADDR. The client closes first, so that what is left
// behind is on its side, and connects to another server the second time, as two connections that
// are the same in all four of address and port cannot be.
//
TEST_F(ClientLocalAddress, WHEN_local_port_is_given_THEN_it_can_be_connected_from_again)
{
   auto first = io::make_acceptor(context.get_executor());
   auto second = io::make_acceptor(context.get_executor());
   io::listen(first, {io::make_address("127.0.0.2"), 0});
   io::listen(second, {io::make_address("127.0.0.2"), 0});
   const TcpEndpoint local{io::make_address("127.0.0.3"), free_port()};

   std::vector<TcpEndpoint> seen;
   auto serve = [this](TcpAcceptor& acceptor, std::vector<TcpEndpoint>& seen) -> Task<void> {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::accept(acceptor, socket)); // or closed, after a connect has failed
      seen.push_back(*io::remote_endpoint(socket));
      std::array<char, 64> buffer{};
      for (;;)
         if (auto [ec, n] = co_await io::read_some(socket, boost::asio::buffer(buffer)); ec)
            break;
   };
   for (auto* acceptor : {&first, &second})
      launch(context.get_executor(), serve(*acceptor, seen), [](const std::exception_ptr&) {});

   std::vector<error_code> results;
   auto client = [this](std::array<TcpAcceptor*, 2> acceptors, TcpEndpoint local,
                        std::vector<error_code>& results) -> Task<void> {
      for (auto* acceptor : acceptors)
      {
         auto socket = io::make_socket(context.get_executor());
         auto [ec, endpoint] = co_await io::connect(socket, {io::local_endpoint(*acceptor)},
                                                    local.address(), local.port());
         results.push_back(ec);
         if (ec)
            io::close(*acceptor); // or its accept() would wait for ever
      } // closing the socket first
   };
   launch(context.get_executor(), client({&first, &second}, local, results),
          [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });
   context.run();

   EXPECT_THAT(results, ElementsAre(error_code{}, error_code{}));
   EXPECT_THAT(seen, ElementsAre(local, local));
}

//
// What a local address can go wrong with is the same over TCP and QUIC: one that is not this
// host's, one of a family that none of the server's endpoints is of, and one that is not an
// address at all.
//
class ClientLocalAddressTls : public ClientTls
{
protected:
   void configure_client(client::Config& config) override { config.local_address = local; }

   std::string local = "127.0.0.3";
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressTls, ClientLocalAddressTls, all_protocols,
                         NameGenerator);

TEST_P(ClientLocalAddressTls, WHEN_local_address_is_on_this_host_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam());
}

class ClientLocalAddressAnyIPv4 : public ClientLocalAddressTls
{
protected:
   ClientLocalAddressAnyIPv4() { local = "0.0.0.0"; }
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressAnyIPv4, ClientLocalAddressAnyIPv4, all_protocols,
                         NameGenerator);

TEST_P(ClientLocalAddressAnyIPv4, WHEN_local_address_is_any_ipv4_one_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), error_code{});
   EXPECT_EQ(protocol, GetParam());
}

class ClientLocalAddressNotOurs : public ClientLocalAddressTls
{
protected:
   ClientLocalAddressNotOurs() { local = "192.0.2.1"; } // TEST-NET-1
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressNotOurs, ClientLocalAddressNotOurs, all_protocols,
                         NameGenerator);

TEST_P(ClientLocalAddressNotOurs, WHEN_local_address_is_not_this_hosts_THEN_connect_fails)
{
   const auto ec = connect();
   EXPECT_EQ(ec, errc::address_not_available) << ec.message();
   EXPECT_FALSE(protocol);
}

//
// The server is on 127.0.0.2, IPv4 only, and so "::" -- any address, but of IPv6 -- has no
// endpoint to go to, no more than "::1" has.
//
class ClientLocalAddressOtherFamily : public ClientLocalAddressTls
{
protected:
   ClientLocalAddressOtherFamily() { local = "::1"; }
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressOtherFamily, ClientLocalAddressOtherFamily,
                         all_protocols, NameGenerator);

TEST_P(ClientLocalAddressOtherFamily, WHEN_no_endpoint_is_of_its_family_THEN_connect_fails)
{
   const auto ec = connect();
   EXPECT_EQ(ec, errc::address_family_not_supported) << ec.message();
   EXPECT_FALSE(protocol);
}

class ClientLocalAddressAnyIPv6 : public ClientLocalAddressTls
{
protected:
   ClientLocalAddressAnyIPv6() { local = "::"; }
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressAnyIPv6, ClientLocalAddressAnyIPv6, all_protocols,
                         NameGenerator);

TEST_P(ClientLocalAddressAnyIPv6, WHEN_any_ipv6_address_and_server_is_ipv4_THEN_connect_fails)
{
   const auto ec = connect();
   EXPECT_EQ(ec, errc::address_family_not_supported) << ec.message();
   EXPECT_FALSE(protocol);
}

class ClientLocalAddressInvalid : public ClientLocalAddressTls
{
protected:
   ClientLocalAddressInvalid() { local = "localhost"; } // a name, not an address
};

INSTANTIATE_TEST_SUITE_P(ClientLocalAddressInvalid, ClientLocalAddressInvalid, all_protocols,
                         NameGenerator);

TEST_P(ClientLocalAddressInvalid, WHEN_local_address_is_not_an_address_THEN_connect_fails)
{
   const auto ec = connect();
   EXPECT_EQ(ec, errc::invalid_argument) << ec.message();
   EXPECT_FALSE(protocol);
}

// =================================================================================================
