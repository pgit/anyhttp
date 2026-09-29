#include "test_fixtures.hpp"

using namespace testing;

// =================================================================================================

class ClientConnect : public Test
{
public:
   void SetUp() override { setup_logging(); }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ClientConnect, WHEN_unknown_host_THEN_completes_with_host_not_found_eventually)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://this-domain-does-not-exist:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session) {
      loge("ERROR: {}", ec.message());
      EXPECT_TRUE(ec == boost::asio::error::netdb_errors::host_not_found ||
                  ec == boost::asio::error::netdb_errors::host_not_found_try_again);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_wrong_port_THEN_completes_with_connection_refused)
{
   boost::asio::io_context context;
   auto port = get_unused_port(context);
   client::Config config{.url = boost::urls::url("http://localhost").set_port_number(port)};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session) {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::connection_refused);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_async_connect_is_cancelled_THEN_returns_operation_canceled)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://localhost:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect(cancel_after(0ms, [this](boost::system::error_code ec, Session session) {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
   }));

   context.run();
}

TEST_F(ClientConnect, WHEN_connect_to_broadcast_ip_THEN_completes_with_network_unreachable)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://255.255.255.255:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session) {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::network_unreachable);
   });
   context.run();
}

// =================================================================================================

//
// The HTTP/3 client verifies the server's certificate, see client::Config::tls_ca_file. Every
// other HTTP/3 test connects successfully with the test PKI's root CA; these make sure that
// without it, or with a host the certificate is not issued for, it doesn't.
//
class ClientTls : public Client
{
protected:
   boost::system::error_code connect()
   {
      boost::system::error_code result;
      client->async_connect([&](boost::system::error_code ec, Session) {
         result = ec;
         server.reset();
      });
      run();
      return result;
   }
};

INSTANTIATE_TEST_SUITE_P(ClientTls, ClientTls, Values(anyhttp::Protocol::h3), NameGenerator);

TEST_P(ClientTls, WHEN_server_certificate_verifies_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), boost::system::error_code{});
}

// -------------------------------------------------------------------------------------------------

class ClientTlsDefaultTrust : public ClientTls
{
protected:
   void configure_client(client::Config& config) override { config.tls_ca_file.clear(); }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsDefaultTrust, ClientTlsDefaultTrust,
                         Values(anyhttp::Protocol::h3), NameGenerator);

TEST_P(ClientTlsDefaultTrust, WHEN_issuer_is_not_trusted_THEN_connect_fails)
{
   EXPECT_EQ(connect(), boost::system::errc::connection_refused);
}

// -------------------------------------------------------------------------------------------------

//
// The test certificate is issued for localhost, ::1, 127.0.0.1 and 127.0.0.2 -- not 127.0.0.3.
//
class ClientTlsWrongHost : public ClientTls
{
protected:
   ClientTlsWrongHost() { url = boost::urls::url{"http://127.0.0.3/custom"}; }

   void configure_server(server::Config& config) override { config.listen_address = "127.0.0.3"; }
};

INSTANTIATE_TEST_SUITE_P(ClientTlsWrongHost, ClientTlsWrongHost, Values(anyhttp::Protocol::h3),
                         NameGenerator);

TEST_P(ClientTlsWrongHost, WHEN_certificate_is_for_another_host_THEN_connect_fails)
{
   EXPECT_EQ(connect(), boost::system::errc::connection_refused);
}

// =================================================================================================
