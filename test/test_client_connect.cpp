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
#if ANYHTTP_CAPY
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
   boost::asio::io_context io;
   auto port = get_unused_port(io);
   EXPECT_EQ(connect(boost::urls::url("http://localhost").set_port_number(port)),
             errc::connection_refused);
}

#if ANYHTTP_CAPY
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
// The HTTP/3 client verifies the server's certificate, see client::Config::tls_ca_file. Every
// other HTTP/3 test connects successfully with the test PKI's root CA; these make sure that
// without it, or with a host the certificate is not issued for, it doesn't.
//
class ClientTls : public Client
{
protected:
   error_code connect()
   {
      error_code result;
      launch(client->get_executor(), connect(this, result));
      run();
      return result;
   }

   static Task<void> connect(ClientTls* self, error_code& result)
   {
      auto [ec, session] = co_await self->client->connect();
      result = ec;
      self->server.reset();
   }
};

INSTANTIATE_TEST_SUITE_P(ClientTls, ClientTls, Values(anyhttp::Protocol::h3), NameGenerator);

TEST_P(ClientTls, WHEN_server_certificate_verifies_THEN_connect_succeeds)
{
   EXPECT_EQ(connect(), error_code{});
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
   EXPECT_EQ(connect(), errc::connection_refused);
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
   EXPECT_EQ(connect(), errc::connection_refused);
}

// =================================================================================================
