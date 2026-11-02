#include "test_fixtures.hpp"

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
// The test certificate is issued for localhost, ::1, 127.0.0.1 and 127.0.0.2 -- not 127.0.0.3.
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
