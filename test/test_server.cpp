#include "test_fixtures.hpp"

#include <array>
#include <future>
#include <optional>
#include <print>
#include <thread>

// =================================================================================================

INSTANTIATE_TEST_SUITE_P(Server, Server,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2),
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
class Http3IdleTimeout : public testing::Test
{
protected:
   static constexpr auto IdleTimeout = 500ms;

   void SetUp() override
   {
      setupLogging();

      server.emplace(context.get_executor(), server::Config{.listen_address = "127.0.0.2",
                                                            .port = 0,
                                                            .idle_timeout = IdleTimeout});
      server->setRequestHandler(
         [this](server::Request request, server::Response response) -> awaitable<void>
      {
         co_await response.async_submit(200, {});

         //
         // Wait for a request body that never comes: this first read is where the handler is
         // suspended when the client freezes, and it must be resumed -- with an error -- once
         // the server gives up on the connection.
         //
         std::array<uint8_t, 1024> buffer;
         auto [ec, n] = co_await request.async_read_some(asio::buffer(buffer), as_tuple);
         handler_result.set_value(ec);
      });

      url.set_port_number(server->local_endpoint().port());
   }

   asio::io_context context;
   std::optional<server::Server> server;

   std::promise<boost::system::error_code> handler_result;
   boost::urls::url url{"http://127.0.0.2/echo"};
};

// -------------------------------------------------------------------------------------------------

TEST_F(Http3IdleTimeout, WHEN_client_vanishes_in_flight_THEN_idle_timer_drops_the_session)
{
   auto result = handler_result.get_future();

   //
   // The server has to keep running while the client is frozen, so it gets a thread of its own.
   //
   std::jthread server_thread([this] { run(context); });
   boost::scope::scope_exit stop_server([this] { context.stop(); });

   //
   // The client runs on its own io_context, which is what makes freezing it possible: stopping
   // that context takes the client off the air mid-request without unwinding anything, so no
   // CONNECTION_CLOSE is ever sent -- just like a client process that was killed.
   //
   asio::io_context client_context;
   client::Client client(client_context.get_executor(),
                         client::Config{.url = url, .protocol = anyhttp::Protocol::h3});

   //
   // Session, request and response are kept out here rather than in the coroutine frame, which is
   // destroyed as soon as the coroutine below returns: unwinding them would reset the stream, and
   // that is a packet -- the one thing this client must not send.
   //
   std::optional<Session> session;
   std::optional<client::Request> request;
   std::optional<client::Response> response;

   bool responded = false;
   co_spawn(client_context, [&]() -> awaitable<void>
   {
      session = co_await client.async_connect();
      request = co_await session->async_submit(url, {});
      response = co_await request->async_get_response();
      responded = true;
   }, detached);

   //
   // Run the client just far enough to have the request open and answered, then stop running it:
   // from here on it never touches its socket again.
   //
   while (client_context.run_one() && !responded);
   ASSERT_TRUE(responded) << "client never received a response";
   std::println("=== freezing the client, request still in flight ===");

   //
   // From here on the server is on its own. Without the idle timer dropping the connection, the
   // request handler stays suspended in async_read_some() forever, and the session it belongs to
   // sits in the server's connection table for good.
   //
   ASSERT_EQ(result.wait_for(5s), std::future_status::ready) << "request handler never completed";
   EXPECT_EQ(result.get(), boost::system::errc::connection_reset);

   //
   // Only now, on the way out, is the frozen client allowed to unwind: doing so earlier would
   // have sent the CONNECTION_CLOSE that this test is all about not sending.
   //
}

// =================================================================================================
