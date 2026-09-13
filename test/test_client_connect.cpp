#include "test_fixtures.hpp"

// =================================================================================================

class ClientConnect : public testing::Test
{
public:
   void SetUp() override { setupLogging(); }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ClientConnect, WHEN_unknown_host_THEN_completes_with_host_not_found_eventually)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://this-domain-does-not-exist:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_TRUE(ec == boost::asio::error::netdb_errors::host_not_found ||
                  ec == boost::asio::error::netdb_errors::host_not_found_try_again);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_wrong_port_THEN_completes_with_host_not_found_eventually)
{
   boost::asio::io_context context;
   auto port = get_unused_port(context);
   client::Config config{.url = boost::urls::url("http://localhost").set_port_number(port)};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::connection_refused);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_async_connect_is_cancelled_THEN_returns_operation_aborted)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://localhost:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect(cancel_after(0ms, [this](boost::system::error_code ec, Session session)
   {
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
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::network_unreachable);
   });
   context.run();
}

// =================================================================================================
