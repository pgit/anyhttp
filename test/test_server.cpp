#include "test_fixtures.hpp"

#include <boost/scope/scope_exit.hpp>

#include <array>
#include <future>
#include <optional>
#include <print>
#include <thread>

using namespace testing;

// =================================================================================================

INSTANTIATE_TEST_SUITE_P(Server, Server, Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(Server, StopBeforeStarted)
{
   server.reset();
   run();
}

TEST_P(Server, Stop)
{
   context.run_one();
   server.reset();
   run();
}

// =================================================================================================

//
// A QUIC peer that goes away without a word -- a killed client, a machine that went to sleep in
// the middle of a request -- leaves the server nothing to react to: no CONNECTION_CLOSE arrives,
// and no further packet ever will. Only the idle timer can notice, and dropping the connection
// when it fires is what releases the session, its streams, and the request handlers suspended on
// them.
//
// Note that this is *not* what a client calling Session::reset() looks like: that one says
// goodbye, and the server cleans up right away by way of the draining period.
//
class Http3IdleTimeout : public Test
{
protected:
   static constexpr auto IdleTimeout = 500ms;

   void SetUp() override
   {
      setup_logging();

      server.emplace(
         context.get_executor(),
         server::Config{.listen_address = "127.0.0.2", .port = 0, .idle_timeout = IdleTimeout});
      server->on_request([this](server::Request request, server::Response response) -> Task<void> {
         check(co_await response.submit(200, {}));

         //
         // Wait for a request body that never comes: this first read is where the handler is
         // suspended when the client freezes, and it must be resumed -- with an error -- once
         // the server gives up on the connection.
         //
         std::array<uint8_t, 1024> buffer;
         auto [ec, n] = co_await request.read_some(asio::buffer(buffer));
         handler_result.set_value(ec);
      });

      url.set_port_number(server->local_endpoint().port());
   }

   IoContext context;
   std::optional<server::Server> server;

   std::promise<error_code> handler_result;
   boost::urls::url url{"http://127.0.0.2/echo"};
};

// -------------------------------------------------------------------------------------------------

TEST_F(Http3IdleTimeout, WHEN_client_vanishes_in_flight_THEN_idle_timer_drops_the_session)
{
   auto result = handler_result.get_future();

   //
   // The server has to keep running while the client is frozen, so it gets a thread of its own.
   //
   std::jthread server_thread([this] { context.run(); });
   boost::scope::scope_exit stop_server([this] { context.stop(); });

   //
   // The client runs on its own io_context, which is what makes freezing it possible: stopping
   // that context takes the client off the air mid-request without unwinding anything, so no
   // CONNECTION_CLOSE is ever sent -- just like a client process that was killed.
   //
   IoContext client_context;
   client::Client client(client_context.get_executor(),
                         client::Config{.url = url,
                                        .protocol = anyhttp::Protocol::h3,
                                        .tls_ca_file = "pki/out/root.pem"});

   //
   // Session, request and response are kept out here rather than in the coroutine frame, which is
   // destroyed as soon as the coroutine below returns: unwinding them would reset the stream, and
   // that is a packet -- the one thing this client must not send.
   //
   std::optional<Session> session;
   std::optional<client::Request> request;
   std::optional<client::Response> response;

   bool responded = false;
   auto client_main = [&]() -> Task<void> {
      session = check(co_await client.connect());
      request = check(co_await session->submit(url, {}));
      response = check(co_await request->get_response());
      responded = true;
   };
   launch(client_context.get_executor(), client_main());

   //
   // Run the client just far enough to have the request open and answered, then stop running it:
   // from here on it never touches its socket again.
   //
   while (client_context.run_one() && !responded)
      ;
   ASSERT_TRUE(responded) << "client never received a response";
   std::println("=== freezing the client, request still in flight ===");

   //
   // From here on the server is on its own. Without the idle timer dropping the connection, the
   // request handler stays suspended in async_read_some() forever, and the session it belongs to
   // sits in the server's connection table for good.
   //
   ASSERT_EQ(result.wait_for(5s), std::future_status::ready) << "request handler never completed";
   EXPECT_EQ(result.get(), errc::connection_reset);

   //
   // Only now, on the way out, is the frozen client allowed to unwind: doing so earlier would
   // have sent the CONNECTION_CLOSE that this test is all about not sending.
   //
}

// =================================================================================================

//
// The server reads its certificate chain and key when it is constructed (see
// server::Config::tls_certificate_chain), so a wrong path shows up right there and not with the
// first TLS connection.
//
TEST(ServerTls, WHEN_certificate_chain_is_missing_THEN_constructor_throws)
{
   IoContext context;
   EXPECT_ANY_THROW(server::Server(context.get_executor(),
                                   server::Config{.listen_address = "127.0.0.2",
                                                  .port = 0,
                                                  .tls_certificate_chain = "pki/out/missing.pem"}));
}

TEST(ServerTls, WHEN_private_key_is_missing_THEN_constructor_throws)
{
   IoContext context;
   EXPECT_ANY_THROW(server::Server(context.get_executor(),
                                   server::Config{.listen_address = "127.0.0.2",
                                                  .port = 0,
                                                  .tls_private_key = "pki/out/missing.pem"}));
}

// =================================================================================================
