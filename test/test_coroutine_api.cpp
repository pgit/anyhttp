#include "test_fixtures.hpp"

#include <string>

using namespace testing;

namespace http = boost::beast::http;

// =================================================================================================

//
// The coroutine spelling of the API -- read_some(), write(), write_eof(), submit(), get_response(),
// connect(), get() -- which both runtimes have: results as tuples, errors reported instead of
// thrown. The test bodies use nothing else, so that they carry over to the other runtime as they
// are.
//
class CoroutineApi : public ClientAsync
{
};

INSTANTIATE_TEST_SUITE_P(CoroutineApi, CoroutineApi, ValuesIn(protocols()), NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(CoroutineApi, WHEN_exchanging_a_body_THEN_both_sides_see_all_of_it)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      std::string body;
      std::array<char, 1024> buffer;
      for (;;)
      {
         auto [ec, n] = co_await request.read_some(asio::buffer(buffer));
         body.append(buffer.data(), n);
         if (ec == errors::eof)
            break;
         EXPECT_FALSE(ec) << ec.message();
         if (ec)
            co_return;
      }

      auto [submit_ec] = co_await response.submit(200, {});
      EXPECT_FALSE(submit_ec) << submit_ec.message();
      auto [write_ec] = co_await response.write(asio::buffer(body));
      EXPECT_FALSE(write_ec) << write_ec.message();
      //
      // Not checked: over HTTP/3, the server's writes complete when the peer acknowledges them,
      // and a client that has read all of the response may close the connection before it
      // acknowledges the last of it.
      //
      std::ignore = co_await response.write_eof(asio::buffer("!", 1));
   };

   clientSession = [this](Session session) -> Task<void> {
      auto [ec, request] = co_await session.submit(url);
      EXPECT_FALSE(ec) << ec.message();

      auto [write_ec] = co_await request.write(asio::buffer("Hello, ", 7));
      EXPECT_FALSE(write_ec) << write_ec.message();
      auto [eof_ec] = co_await request.write_eof(asio::buffer("World", 5));
      EXPECT_FALSE(eof_ec) << eof_ec.message();

      auto [response_ec, response] = co_await request.get_response();
      EXPECT_FALSE(response_ec) << response_ec.message();
      EXPECT_EQ(response.status_code(), 200);

      std::string body;
      std::array<char, 1024> buffer;
      for (;;)
      {
         auto [ec, n] = co_await response.read_some(asio::buffer(buffer));
         body.append(buffer.data(), n);
         if (ec)
         {
            EXPECT_EQ(ec, errors::eof);
            break;
         }
      }
      EXPECT_EQ(body, "Hello, World!");
   };
}

TEST_P(CoroutineApi, WHEN_writing_after_the_end_THEN_error_is_reported_not_thrown)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      std::ignore = co_await response.submit(200, {});
      std::ignore = co_await response.write_eof();
   };

   clientSession = [this](Session session) -> Task<void> {
      auto [ec, request] = co_await session.submit(url);
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_FALSE(std::get<0>(co_await request.write_eof()));

      auto [write_ec] = co_await request.write(asio::buffer("late", 4));
      EXPECT_EQ(write_ec, errc::broken_pipe);

      auto [response_ec, response] = co_await request.get_response();
      EXPECT_FALSE(response_ec) << response_ec.message();
      EXPECT_EQ(co_await drain(response), 0);
   };
}

//
// A body may end, and its stream close, before the reader comes back for the rest of it -- which
// has to be there when it does. (HTTP/2 used to drop the stream with the data it held: with ASIO,
// a reader that is resumed inline usually kept up; CAPY, which resumes it later, always lost it.)
//
constexpr size_t small_body = 16 * 1024; // well within the flow control window

TEST_P(CoroutineApi, WHEN_reader_comes_back_after_the_body_has_ended_THEN_all_of_it_is_there)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      std::string body(small_body, 'x');
      std::ignore = co_await response.submit(200, fields({{"Content-Length", small_body}}));
      std::ignore = co_await response.write_eof(asio::buffer(body));
   };

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());

      std::array<char, 1024> buffer;
      auto n = check(co_await response.read_some(asio::buffer(buffer)));

      co_await sleep(100ms); // meanwhile, the rest of the body arrives, and the stream closes
      EXPECT_EQ(n + co_await drain(response), small_body);
   };
}

TEST_P(CoroutineApi, WHEN_connecting_and_getting_THEN_message_arrives)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      std::ignore = co_await response.submit(200, fields({{"Content-Length", 2}}));
      std::ignore = co_await response.write_eof(asio::buffer("ok", 2));
   };

   clientSession = [this](Session) -> Task<void> {
      auto [ec, session] = co_await client->connect();
      EXPECT_FALSE(ec) << ec.message();

      auto [get_ec, message] = co_await session.get(url);
      EXPECT_FALSE(get_ec) << get_ec.message();
      EXPECT_EQ(message.result(), http::status::ok);
      EXPECT_EQ(message.body(), "ok");
   };
}

// =================================================================================================
