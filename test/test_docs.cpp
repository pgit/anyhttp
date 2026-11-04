//
// Runs the code of the programmer's guide, docs/snippets.cpp, so that what the guide says it does
// is what it does. It compiling at all is seen to by the documentation's own object library.
//
#include "test_fixtures.hpp"

#include "../docs/snippets.cpp"

#include <gtest/gtest.h>

using namespace anyhttp;
using namespace testing;
using namespace std::chrono_literals;

class Guide : public ClientAsync
{
};

INSTANTIATE_TEST_SUITE_P(Guide, Guide, Values(Protocol::h1, Protocol::h2, Protocol::h3),
                         NameGenerator);

// =================================================================================================

TEST_P(Guide, Route)
{
   server->on_request(snippets::route);
   clientSession = [this](Session session) -> Task<void> {
      auto repeat = url;
      repeat.set_path("/repeat");
      repeat.set_encoded_query("count=5");
      auto [repeat_ec, repeated] = co_await session.get(repeat);
      EXPECT_FALSE(repeat_ec) << repeat_ec.message();
      EXPECT_EQ(repeated.body(), "*****");

      auto [echo_ec, echoed] = co_await snippets::post(session, url.set_path("/echo"), "hello");
      EXPECT_FALSE(echo_ec) << echo_ec.message();
      EXPECT_EQ(echoed, "hello");

      auto [missing_ec, missing] = co_await session.get(url.set_path("/missing"));
      EXPECT_FALSE(missing_ec) << missing_ec.message();
      EXPECT_EQ(missing.result_int(), 404);
   };
}

TEST_P(Guide, EchoAndPost)
{
   requestHandler = snippets::serve_echo;
   clientSession = [this](Session session) -> Task<void> {
      auto [ec, body] = co_await snippets::post(session, url, "hello");
      EXPECT_FALSE(ec);
      EXPECT_EQ(body, "hello");
   };
}

TEST_P(Guide, Get)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      co_await snippets::respond_with(std::move(response), "Hello, World!");
   };
   clientSession = [this](Session session) -> Task<void> {
      const auto body = co_await snippets::get_body(session, url);
      EXPECT_EQ(body, "Hello, World!");
   };
}

TEST_P(Guide, ReadAtMost)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      co_await snippets::respond_with(std::move(response), std::string(1024 * 1024, '*'));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto total = co_await snippets::read_at_most(std::move(session), url, 1024);
      EXPECT_GE(total, 1024);
      EXPECT_LT(total, 1024 * 1024);
   };
}

//
// More than fits into the flow control windows and socket buffers, which is what the guide says
// takes writing and reading at the same time.
//
TEST_P(Guide, RoundTrip)
{
   requestHandler = snippets::serve_echo;
   clientSession = [this](Session session) -> Task<void> {
      const std::string body(1024 * 1024, '*');
      const auto received = co_await snippets::round_trip(session, url, body);
      EXPECT_EQ(received, body);
   };
}

TEST_P(Guide, GetOrGiveUp)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      std::ignore = co_await delay(200ms);
      co_await snippets::respond_with(std::move(response), "too late");
   };
   clientSession = [this](Session session) -> Task<void> {
      auto message = co_await snippets::get_or_give_up(session, url, 50ms);
      EXPECT_FALSE(message);
   };
}

TEST_P(Guide, SubmitPatiently)
{
   requestHandler = snippets::serve_echo;
   clientSession = [this](Session session) -> Task<void> {
      auto [ec, request] = co_await snippets::submit_patiently(session, url);
      EXPECT_FALSE(ec);
      std::ignore = co_await request.write_eof();
      auto [response_ec, response] = co_await request.get_response();
      EXPECT_FALSE(response_ec);
      auto [read_ec, body] = co_await snippets::read_all(response);
      EXPECT_FALSE(read_ec);
   };
}

TEST_P(Guide, Shutdown)
{
   requestHandler = snippets::serve_echo;
   clientSession = [this](Session session) -> Task<void> {
      co_await snippets::say_goodbye(std::move(session));
   };
}

#if ANYHTTP_ASIO
TEST_P(Guide, Tokens)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      co_await snippets::respond_with(std::move(response), "tokens");
   };
   clientSession = [this](Session session) -> Task<void> {
      const auto thrown = co_await snippets::token_exception(session, url);
      EXPECT_EQ(thrown, "tokens");
      const auto reported = co_await snippets::token_tuple(session, url);
      EXPECT_EQ(reported, "tokens");
   };
}

TEST_P(Guide, CancelAfter)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      std::ignore = co_await delay(200ms);
      co_await snippets::respond_with(std::move(response), "too late");
   };
   clientSession = [this](Session session) -> Task<void> {
      auto [ec, request] = co_await session.submit(url);
      EXPECT_FALSE(ec);
      std::ignore = co_await request.write_eof();
      const auto answered = co_await snippets::response_within(request, 50ms);
      EXPECT_FALSE(answered);
   };
}
#endif

// -------------------------------------------------------------------------------------------------

TEST(GuideRunning, Stop) { snippets::run_for_a_while({.listen_address = "127.0.0.2", .port = 0}); }

// not on COROSIO: docs/corosio-port-plan.md, step 8, has why this is for the threaded TSAN tree
// only
#if !ANYHTTP_COROSIO
TEST(GuideRunning, Threads)
{
   snippets::run_on_threads({.listen_address = "127.0.0.2", .port = 0}, 4);
}
#endif

// -------------------------------------------------------------------------------------------------

TEST(GuideQuic, Echo)
{
   snippets::IoContext context;

   std::optional<server::Server> server;
   server.emplace(context.get_executor(), server::Config{.listen_address = "127.0.0.2", .port = 0});
   snippets::quic_serve(*server);

   const auto port = std::to_string(server->local_endpoint().port());

   std::string received;
   launch(context.get_executor(),
          [](Executor executor, std::string port, std::string& received,
             std::optional<server::Server>& server) -> Task<void> {
             auto [ec, body] =
                co_await snippets::quic_hello(executor, "127.0.0.2", port, "pki/out/root.pem");
             EXPECT_FALSE(ec) << ec.message();
             received = std::move(body);
             server.reset();
          }(context.get_executor(), port, received, server));

   context.run();
   EXPECT_EQ(received, "hello");
}
