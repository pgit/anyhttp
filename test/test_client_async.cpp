#include "test_fixtures.hpp"

#include "anyhttp/net.hpp"

#include <pthread.h>

#include <array>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <span>

using namespace testing;

// =================================================================================================

INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync, ValuesIn(protocols()), NameGenerator);

/// Beast's end_of_stream, as the runtime's error code.
static const error_code end_of_stream =
   boost::system::error_code(boost::beast::http::error::end_of_stream);

/**
 * Cancels \p task after \p timeout, the way each runtime does it: ASIO with cancel_after() on the
 * coroutine spawned for it, CAPY with a stop request. Yields what the task threw.
 */
template <typename Rep, typename Period>
Task<std::exception_ptr> cancel_task_after(std::chrono::duration<Rep, Period> timeout,
                                           Task<void> task)
{
#if ANYHTTP_CAPY
   co_return co_await caught(stop_after(timeout, std::move(task)));
#else
   auto [ep] = co_await co_spawn(co_await this_coro::executor, std::move(task),
                                 cancel_after(timeout, as_tuple));
   co_return ep;
#endif
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_post_data_THEN_receive_echo)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      size_t bytes = 1024;
      auto count = co_await when_both(generate(request, bytes), count_response(request));
      EXPECT_EQ(bytes, count);
   };
}

TEST_P(ClientAsync, WHEN_post_without_path_THEN_error_404)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path(""), {}));
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.get_response();
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("unknown"), {}));
      co_await generate(request, 1_m);
      auto response = check(co_await request.get_response());
      EXPECT_EQ(response.status_code(), 404);
      auto received = co_await drain(response);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_error_500)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("discard"), {}));
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.get_response();
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_delayed_THEN_error_500)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("detach"), {}));
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.get_response();
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_with_body_delayed_THEN_error_500)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("detach"), {}));
      auto ep = co_await caught(send(request, rv::iota(uint8_t{0})));
      EXPECT_TRUE(ep);
   };
}

TEST_P(ClientAsync, WHEN_invalid_port_in_host_header_THEN_reports_error)
{
   clientSession = [this](Session session) -> Task<void> {
      Fields fields;
      fields.set("Host", "host:12345x");
      auto request = check(co_await session.submit(url.set_path("echo"), fields));
      auto response = co_await when_both(send_eof(request), count_response(request));
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_called_twice_THEN_reports_error)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo")));
      auto [ec, response] = co_await request.get_response();
      EXPECT_FALSE(ec) << what(ec);
      std::tie(ec, response) = co_await request.get_response();
      EXPECT_EQ(ec, errc::connection_already_in_progress);
      EXPECT_EQ(ec, errors::already_started);
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_detached_THEN_does_not_crash)
{
   if (GetParam() == anyhttp::Protocol::h1)
      GTEST_SKIP();

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo")));
#if ANYHTTP_CAPY
      // with nobody waiting for it: what a detached completion token is to ASIO
      launch(client->get_executor(), [](client::Request request) -> Task<void> {
         std::ignore = co_await request.get_response();
      }(std::move(request)));
#else
      request.async_get_response(detached);
#endif
   };
}

// -------------------------------------------------------------------------------------------------

//
// Requests and responses may outlive the session they belong to. Once it is gone, there is no
// connection left for them to use: whatever they are asked to do has to complete with an error,
// without touching what used to be the session's stream.
//
// On the client side, only an HTTP/1.1 session is gone as soon as it is reset(). HTTP/2 and HTTP/3
// sessions shut down asynchronously, so operations may still complete successfully for a little
// while, with data that has already arrived.
//
static bool is_connection_error(const error_code& ec)
{
   return ec == errc::connection_aborted || ec == errc::connection_reset;
}

TEST_P(ClientAsync, WHEN_session_is_gone_THEN_request_reports_error)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP();

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      session.reset();

      auto [ec] = co_await request.write(asio::buffer("Hello"sv));
      EXPECT_TRUE(is_connection_error(ec)) << what(ec);

      std::tie(ec) = co_await request.write_eof();
      EXPECT_TRUE(is_connection_error(ec)) << what(ec);

      auto [ec2, response] = co_await request.get_response();
      EXPECT_TRUE(is_connection_error(ec2)) << what(ec2);
   };
}

//
// The two sides hand over explicitly instead of polling: the client says when it is gone, and
// the responder says when it is done. The latter is not just tidiness -- the client coroutine
// returning is what tears the server down, so it must not return early.
//
// One wait resists that treatment. What the responder is really waiting for is its own session
// to be gone, and for HTTP/2 and HTTP/3 that only happens once the server has noticed the closed
// connection -- a network event, which nothing in this process can be woken by. So the sleep
// stays, but it now covers only that, with the client's own teardown fenced off ahead of it.
//
TEST_P(ClientAsync, WHEN_server_session_is_gone_THEN_response_reports_error)
{
   auto clientGone = std::make_shared<Signal>(context.get_executor());
   auto responded = std::make_shared<Signal>(context.get_executor());

   requestHandler = [this, clientGone, responded](server::Request request,
                                                  server::Response response) -> Task<void> {
      //
      // Keep the response around beyond the request handler, until the client has closed the
      // connection and the server session has ended.
      //
      auto respond = [](std::shared_ptr<Signal> clientGone, std::shared_ptr<Signal> responded,
                        server::Response response) -> Task<void> {
         std::ignore = co_await clientGone->wait();
         co_await sleep(100ms); // no signal can stand in for this, see above

         auto [ec] = co_await response.submit(200, {});
         EXPECT_TRUE(is_connection_error(ec)) << what(ec);

         std::tie(ec) = co_await response.write(asio::buffer("Hello"sv));
         EXPECT_TRUE(is_connection_error(ec)) << what(ec);

         responded->notify();
      };
      launch(context.get_executor(), respond(clientGone, responded, std::move(response)));
      co_return;
   };
   clientSession = [this, clientGone, responded](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url, {}));
      check(co_await request.write_eof());
      request.reset();
      session.reset();

      clientGone->notify();
      std::ignore = co_await responded->wait();
   };
}

//
// The head of a request has arrived in full before the handler sees it, so it stays readable
// after the session is gone. A handler may first look at it only then: under load, a server
// shutting down can tear down a session before the handlers of its last requests have run.
//
TEST_P(ClientAsync, WHEN_server_session_is_gone_THEN_request_head_is_still_there)
{
   auto clientGone = std::make_shared<Signal>(context.get_executor());
   auto inspected = std::make_shared<Signal>(context.get_executor());

   requestHandler = [this, clientGone, inspected](server::Request request,
                                                  server::Response response) -> Task<void> {
      //
      // Keep the request beyond the request handler, until the client has closed the connection
      // and the server session has ended. The response, too, so the exchange is still open then.
      //
      auto inspect = [](std::shared_ptr<Signal> clientGone, std::shared_ptr<Signal> inspected,
                        server::Request request, server::Response) -> Task<void> {
         const std::string method(request.method());
         const std::string url(request.url().buffer());

         std::ignore = co_await clientGone->wait();
         co_await sleep(100ms); // see WHEN_server_session_is_gone_THEN_response_reports_error

         EXPECT_EQ(request.method(), method);
         EXPECT_EQ(request.url().buffer(), url);
         EXPECT_EQ(request.url().path(), "/custom/head");
         EXPECT_EQ(request.fields()["X-Test"], "42");

         inspected->notify();
      };
      launch(context.get_executor(),
             inspect(clientGone, inspected, std::move(request), std::move(response)));
      co_return;
   };
   clientSession = [this, clientGone, inspected](Session session) -> Task<void> {
      Fields fields;
      fields.set("X-Test", "42");
      auto request = check(co_await session.submit(url.set_path("custom/head"), fields));
      check(co_await request.write_eof());
      request.reset();
      session.reset();

      clientGone->notify();
      std::ignore = co_await inspected->wait();
   };
}

//
// With HTTP/1.1 pipelining, a session has more than one request at a time. Releasing the earlier
// one must not make the session forget about the later one, which has to learn about the session
// going away all the same.
//
TEST_P(ClientAsync,
       WHEN_earlier_request_is_released_THEN_later_request_still_learns_session_is_gone)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP();

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer("Hello, Server #1!"sv)));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));

      request1.reset();
      session.reset();

      auto [ec] = co_await request2.write(asio::buffer("Hello"sv));
      EXPECT_TRUE(is_connection_error(ec)) << what(ec);
   };
}

TEST_P(ClientAsync, WHEN_session_is_gone_THEN_earlier_request_reports_error)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP();

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer("Hello, Server #1!"sv)));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));

      session.reset();

      auto [ec, response] = co_await request1.get_response();
      EXPECT_TRUE(is_connection_error(ec)) << what(ec);
   };
}

TEST_P(ClientAsync,
       WHEN_earlier_response_is_released_THEN_later_response_still_learns_session_is_gone)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP();

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer("Hello, Server #1!"sv)));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write(asio::buffer("Hello, Server #2!"sv)));

      auto response1 = check(co_await request1.get_response());
      EXPECT_EQ(co_await drain(response1), 17);
      auto response2 = check(co_await request2.get_response());

      response1.reset();
      session.reset();

      //
      // The body of the second response is still open, as its request has not been ended.
      //
      std::array<char, 64> buffer;
      auto [ec, n] = co_await response2.read_some(asio::buffer(buffer));
      EXPECT_EQ(ec, errors::partial_message) << what(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_while_writing_THEN_connection_is_reset)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await sleep(150ms);
      request.reset();
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      auto ep = co_await caught(send(request, rv::iota(uint8_t(0))));

      //
      // Which of the two it is depends on where the teardown catches the write: the RST that
      // follows the server's FIN fails the write in progress with ECONNRESET, and every one
      // after it with EPIPE.
      //
      EXPECT_THAT(code(ep), AnyOf(errc::connection_reset, //
                                  errc::broken_pipe))
         << what(ep);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_and_response_THEN_completes_anyway)
{
   // if (GetParam() == anyhttp::Protocol::h1)
   //    GTEST_SKIP(); // FIXME: timeout

   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      auto [ec, _] = co_await request.get_response();
      EXPECT_EQ(ec, end_of_stream);
      // EXPECT_EQ(ec, std::errc::connection_reset);
   };
}

TEST_P(ClientAsync, WHEN_client_cancels_write_THEN_can_resume)
{
   if (GetParam() == anyhttp::Protocol::h1)
      GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo")));
      auto response = check(co_await request.get_response());

      // send as much data as possible within 1s, should run into backpressure
      auto ep = co_await cancel_task_after(1s, send(request, rv::iota(uint8_t(0))));
      EXPECT_EQ(code(ep), errc::operation_canceled);

      if (GetParam() == anyhttp::Protocol::h3)
      {
         //
         // QUIC: whether the FIN can slip out while the send window is closed depends on flow
         // control timing, so don't assert either way here. What matters is that ending the
         // upload and draining the response together complete the exchange.
         //
         auto received = co_await when_both(send_eof(request), drain(response));
         EXPECT_GT(received, 0);
      }
      else
      {
         // now, with a closed window, we cannot even end the upload
         ep = co_await cancel_task_after(1ms, send_eof(request));
         EXPECT_EQ(code(ep), errc::operation_canceled);

         // as we have no control over when the send window is re-opened, wait for it in parallel
         auto received = co_await when_both(send_eof(request), drain(response));
         EXPECT_GT(received, 0);
      }
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, YieldFuzz)
{
#if 0
   static std::random_device rd;
   static std::mt19937 gen(rd());
#else
   static std::mt19937 gen(42); // fixed seed for reproducibility
#endif

   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      std::uniform_int_distribution<> dist(0, 10);
      constexpr auto msg = "Hello, Client!"sv;
      co_await yield(dist(gen));
      Fields fields;
      fields.set("Content-Length", std::to_string(msg.size()));
      check(co_await response.submit(200, fields));
      co_await yield(dist(gen));
      check(co_await response.write(asio::buffer(msg)));
      co_await yield(dist(gen));
      check(co_await response.write_eof());
      co_await yield(dist(gen));
      std::array<uint8_t, 16> data;
      co_await request.read_some(asio::buffer(data));
   };
   clientSession = [this](Session session) -> Task<void> {
      std::uniform_int_distribution<> dist(0, 10);
      for (size_t i = 0; i < 100; ++i)
      {
         std::println(
            "=== {} =========================================================================", i);
         co_await yield(dist(gen));
         Fields fields;
         if (GetParam() == anyhttp::Protocol::h1)
            fields.set("Connection", "Keep-Alive");
         fields.set("Content-Length", "0");
         auto request = check(co_await session.submit(url, fields));
         co_await yield(dist(gen));
         check(co_await request.write_eof());
         co_await yield(dist(gen));
         co_await count_response(request);
      }
   };
}

//
// The end of an incoming body is an error code, not a zero-sized read -- and it keeps being
// reported for every read issued after it. A zero-length buffer, on the other hand, says nothing
// about the body at all: it completes immediately, at the end of a body just as anywhere else.
//
TEST_P(ClientAsync, WHEN_body_ends_THEN_read_reports_eof)
{
   static const auto hello = "Hello, World!"sv;
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      check(co_await response.submit(200, fields({{"Content-Length", hello.size()}})));
      check(co_await response.write_eof(asio::buffer(hello)));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());

      std::string body;
      std::array<char, 4> buffer; // small on purpose: several reads before the end
      for (;;)
      {
         auto [ec, n] = co_await response.read_some(asio::buffer(buffer));
         if (ec)
         {
            EXPECT_EQ(ec, errors::eof);
            EXPECT_EQ(n, 0u);
            break;
         }
         body.append(buffer.data(), n);
      }
      EXPECT_EQ(body, hello);

      //
      // Reading past the end of a body says the same thing again -- also once the protocol layer
      // has torn the underlying stream down in the meantime, which the yield gives it every
      // opportunity to do (both sides of the exchange are finished by now).
      //
      co_await yield(20);
      auto [ec, n] = co_await response.read_some(asio::buffer(buffer));
      EXPECT_EQ(ec, errors::eof);

      // ... but a zero-length read is not a read, and reports nothing
      std::array<char, 0> empty;
      std::tie(ec, n) = co_await response.read_some(asio::buffer(empty));
      EXPECT_FALSE(ec);
      EXPECT_EQ(n, 0u);
   };
}

//
// A sequence of buffers is read into its first non-empty buffer.
//
TEST_P(ClientAsync, WHEN_reading_into_buffer_sequence_THEN_empty_buffers_are_skipped)
{
   static const auto hello = "Hello, World!"sv;
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      check(co_await response.submit(200, fields({{"Content-Length", hello.size()}})));
      check(co_await response.write_eof(asio::buffer(hello)));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());

      std::array<char, 0> empty;
      std::array<char, 64> buffer;
      auto n =
         check(co_await response.read_some(std::array{asio::buffer(empty), asio::buffer(buffer)}));
      EXPECT_GT(n, 0u);
      EXPECT_THAT(hello, StartsWith(std::string_view(buffer.data(), n)));
   };
}

//
// An empty async_write() no longer ends a body -- async_write_eof() does, and nothing else. So a
// message with an empty write in the middle of it still carries everything written after that.
//
TEST_P(ClientAsync, WHEN_empty_buffer_is_written_THEN_body_stays_open)
{
   static const auto tail = "still here"sv;
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      EXPECT_EQ(co_await drain(request), 0u);
      check(co_await response.submit(200, {}));
      check(co_await response.write({})); // writes nothing, leaves the body open
      check(co_await response.write_eof(asio::buffer(tail)));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write({})); // likewise: the request body stays open
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());
      EXPECT_EQ(co_await read(response), tail);
   };
}

//
// Ending a body twice is harmless -- the second call has nothing left to do -- and an empty
// write stays a free no-op even then. Data after the end is neither: there is no body left for
// it to belong to, through whichever entry point it tries to sneak in.
//
TEST_P(ClientAsync, WHEN_written_after_eof_THEN_reports_broken_pipe)
{
   static const auto hello = "Hello, World!"sv;
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      check(co_await response.submit(200, fields({{"Content-Length", hello.size()}})));
      check(co_await response.write_eof(asio::buffer(hello)));

      auto [ec] = co_await response.write_eof();
      EXPECT_FALSE(ec);

      std::tie(ec) = co_await response.write({});
      EXPECT_FALSE(ec);

      std::tie(ec) = co_await response.write(asio::buffer(hello));
      EXPECT_EQ(ec, errc::broken_pipe);

      std::tie(ec) = co_await response.write_eof(asio::buffer(hello));
      EXPECT_EQ(ec, errc::broken_pipe);
   };
   clientSession = [this](Session session) -> Task<void> {
      EXPECT_EQ((check(co_await session.get(url))).body(), hello);
   };
}

TEST_P(ClientAsync, HelloWorld)
{
   static const auto hello = "Hello, World!"sv;
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      check(co_await response.submit(200, {}));
      check(co_await response.write_eof(asio::buffer(hello)));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message.body(), hello);
   };
}

// -------------------------------------------------------------------------------------------------

//
// A single async_write() larger than what the transport hands to its peer in one go, i.e. the
// whole body in one call instead of chunk by chunk. HTTP/3 has to keep offering the same buffer
// to nghttp3 across many packets here, and complete the write only once all of it is
// acknowledged.
//
TEST_P(ClientAsync, WHEN_server_writes_large_buffer_at_once_THEN_receives_all)
{
   static const std::vector<uint8_t> body = [] {
      std::vector<uint8_t> data(256_k);
      std::ranges::generate(data, [i = uint8_t(0)]() mutable { return i++; });
      return data;
   }();

   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      // drain the request -- HTTP/1.1 closes the connection on an unfinished parser
      co_await drain(request);

      check(co_await response.submit(200, fields({{"Content-Length", body.size()}})));
      check(co_await response.write_eof(asio::buffer(body)));
   };
   clientSession = [this](Session session) -> Task<void> {
      EXPECT_EQ((check(co_await session.get(url))).body().size(), body.size());
   };
}

//
// Cancelling an async_write_eof() that carries data. The buffer goes back to the caller the
// moment the handler runs, so the backend must stop referencing it right there -- for HTTP/3's
// zero-copy path that means resetting the stream, exactly as for a cancelled plain write; under
// ASAN this test is what catches a backend that keeps pointing into the freed buffer.
//
TEST_P(ClientAsync, WHEN_server_cancels_write_eof_THEN_client_sees_truncated_body)
{
   static const std::vector<uint8_t> body(8_m, 'x');

   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      check(co_await response.submit(200, {}));

      //
      // Far more than the peer's receive window, and the client below doesn't read a byte until
      // this is over, so the write is guaranteed to still be in progress when it is cancelled.
      //
#if ANYHTTP_CAPY
      auto [ec] = co_await stop_after(50ms, response.write_eof(asio::buffer(body)));
#else
      auto [ec] =
         co_await response.async_write_eof(asio::buffer(body), cancel_after(50ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());

      // leave the body untouched until the cancellation above has hit, see the sibling testcase
      co_await sleep(150ms);

      error_code ec;
      auto received = co_await try_receive(response, ec);
      EXPECT_LT(received, body.size());
      EXPECT_EQ(ec, errors::partial_message);
   };
}

//
// Cancelling an async_write_eof() whose FIN never made it out must not leave the body in limbo:
// the intent to end it is rolled back, and a re-issued async_write_eof() ends the (now shorter)
// body for real -- instead of completing as a no-op while the peer waits forever for the end.
//
TEST_P(ClientAsync, WHEN_client_cancels_write_eof_THEN_can_still_end)
{
   if (GetParam() == anyhttp::Protocol::h1)
      GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects

   static const std::vector<uint8_t> body(8_m, 'x');

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo")));
      auto response = check(co_await request.get_response());

      // far more than the send window, with nobody reading the echo yet: this cannot complete
      auto write_eof = [&]() -> Task<void> {
         check(co_await request.write_eof(asio::buffer(body)));
      };
      auto ep = co_await cancel_task_after(100ms, write_eof());
      EXPECT_EQ(code(ep), errc::operation_canceled);

      // the FIN never went out with the cancelled write, so the body can still be ended
      auto received = co_await when_both(send_eof(request), drain(response));
      EXPECT_GT(received, 0u);
      EXPECT_LT(received, body.size());
   };
}

//
// Cancelling a response write mid-body. HTTP/3 hands the caller's buffer to nghttp3 by reference,
// so bytes already offered and not yet acknowledged cannot simply be abandoned -- the stream is
// reset instead, which is what the peer would see anyway for a body that stops short of its end.
//
TEST_P(ClientAsync, WHEN_server_cancels_write_THEN_client_sees_truncated_body)
{
   static const std::vector<uint8_t> body(8_m, 'x');

   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      // drain the request -- HTTP/1.1 closes the connection on an unfinished parser
      co_await drain(request);

      check(co_await response.submit(200, {}));

      //
      // Far more than the peer's receive window, and the client below doesn't read a byte until
      // this is over, so the write is guaranteed to still be in progress when it is cancelled.
      //
      auto ep = co_await cancel_task_after(50ms, send(response, std::span(body)));
      EXPECT_EQ(code(ep), errc::operation_canceled);
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());

      //
      // Leave the response body untouched for now: whatever the server manages to send fills up
      // the receive window and stays there, so its write cannot run to completion before the
      // cancellation above hits.
      //
      co_await sleep(150ms);

      error_code ec;
      auto received = co_await try_receive(response, ec);
      std::println("received {} of {} bytes ({})", received, body.size(), ec.message());
      EXPECT_LT(received, body.size());
      EXPECT_EQ(ec, errors::partial_message);
   };
}

// =================================================================================================

TEST_P(ClientAsync, ServerYieldFirst)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await yield(10);
      check(co_await response.submit(200, {}));
      co_await yield(10);
      check(co_await response.write_eof());
   };
   clientSession = [this](Session session) -> Task<void> {
      EXPECT_EQ((check(co_await session.get(url))).result_int(), 200);
   };
}

// ----------------------------------------------------------------------------------------------

static std::optional<size_t> stack_remaining_bytes()
{
   pthread_attr_t attr;
   if (pthread_getattr_np(pthread_self(), &attr) != 0)
      return std::nullopt;

   void* stack_base = nullptr;
   size_t stack_size = 0;
   if (pthread_attr_getstack(&attr, &stack_base, &stack_size) != 0)
   {
      pthread_attr_destroy(&attr);
      return std::nullopt;
   }

   pthread_attr_destroy(&attr);

   if (stack_base == nullptr || stack_size == 0)
      return std::nullopt;

   int local = 0;
   std::uintptr_t sp = reinterpret_cast<std::uintptr_t>(&local);
   std::uintptr_t base = reinterpret_cast<std::uintptr_t>(stack_base);

   return (sp >= base) ? std::optional(sp - base) : std::nullopt;
}

TEST_P(ClientAsync, Recursion)
{
#if __has_feature(address_sanitizer)
   GTEST_SKIP() << "skipped under address sanitizer";
#endif
   if (!stack_remaining_bytes())
      GTEST_SKIP() << "unable to measure stack on this platform";

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      // verify that immediate completion (here, due to an empty buffer) does not cause recursion
      std::array<uint8_t, 0> empty;
      check(co_await response.read_some(asio::buffer(empty)));
      auto s0 = stack_remaining_bytes().value();
      check(co_await response.read_some(asio::buffer(empty)));
      auto s1 = stack_remaining_bytes().value();
      EXPECT_EQ(s0, s1);

#if !ANYHTTP_CAPY
      // however, ASIO allows us to control this behavior using "immediate executors"
      auto ex = co_await this_coro::executor;
      co_await response.async_read_some(asio::buffer(empty), bind_immediate_executor(ex));
      auto s2 = stack_remaining_bytes().value();
      EXPECT_GT(s1, s2);
#endif
   };
}

// ----------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Custom)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      check(co_await response.submit(200, {}));
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         auto [ec, n] = co_await request.read_some(asio::buffer(buffer));
         if (ec)
         {
            check(co_await response.write_eof());
            co_return;
         }
         check(co_await response.write(asio::buffer(buffer, n)));
      }
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url, {}));
      constexpr size_t bytes = 1024;
      auto count = co_await when_both(generate(request, bytes), count_response(request));
      EXPECT_EQ(bytes, count);
   };
}

TEST_P(ClientAsync, IgnoreRequest)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      check(co_await response.submit(200, {}));
      check(co_await response.write_eof());
   };
   clientSession = [this](Session session) -> Task<void> {
      Fields fields;
      fields.set("content-length", "0");
      auto request = check(co_await session.submit(url, fields));
      auto count = co_await when_both(generate(request, 0), count_response(request));
      EXPECT_EQ(count, 0);
   };
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url, {}));
      auto res = co_await when_both(generate(request, 0), try_read_response(request));
      EXPECT_FALSE(res.has_value());
      std::println("ERROR: {}", res.error().message());
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, PostRange)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      // check(co_await request.write(asio::buffer("ping"sv))); // FIXME:
      auto response = check(co_await request.get_response());
      // std::string s(10_m, 'a');
      // auto sender = send(request, std::string_view("blah"));
      // auto sender = send(request, std::string(10_m, 'a'));
      auto sender = send_and_force_eof(request, rv::iota(uint8_t(0)) | rv::take(1_m));
      auto received = co_await when_both(std::move(sender), drain(response));
      loge("received: {}", received);
      EXPECT_EQ(received, 1_m);
   };
}

TEST_P(ClientAsync, PostRangeImmediate)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto sender = send_and_force_eof(request, rv::iota(uint8_t(0)) | rv::take(1_m));
      auto received = co_await when_both(std::move(sender), count_response(request));
      loge("received: {}", received);
      EXPECT_EQ(received, 1_m);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());
      constexpr size_t bytes = 1024;
      co_await generate(request, bytes);
      EXPECT_EQ(co_await drain(response), bytes);
   };
}

// -------------------------------------------------------------------------------------------------

//
// HTTP/1.1 supports pipelining: multiple requests can be made before the responses are received.
// On the wire, requests and responses can not be interleaved, though, so the HTTP/1.1 client puts
// them in order. See ClientSession in h1_session.hpp for the rules; HTTP/2 and HTTP/3 multiplex
// requests and don't need any of them.
//
TEST_P(ClientAsync, WHEN_multiple_request_are_made_THEN_responses_are_received_in_order)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer("Hello, Server #1!"sv)));

      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer("Hello, Server #2! XYZ"sv)));

      auto response1 = check(co_await request1.get_response());
      EXPECT_EQ(co_await drain(response1), 17);

      auto response2 = check(co_await request2.get_response());
      EXPECT_EQ(co_await drain(response2), 21);
   };
}

static constexpr auto body1 = "Hello, Server #1!"sv;
static constexpr auto body2 = "Hello, Server #2! XYZ"sv;

//
// HTTP/1.1 behaves like a protocol with "max concurrent streams = 1": submitting has to wait for
// the previous request to be complete. Instead of waiting, which would deadlock here, it reports
// an error, so that it can be retried later.
//
// TODO: HTTP/2 and HTTP/3 should behave the same way when the peer limits concurrent streams.
//
TEST_P(ClientAsync, WHEN_request_is_submitted_before_previous_is_complete_THEN_reports_would_block)
{
   clientSession = [this](Session session) -> Task<void> {
      const bool limited = GetParam() == anyhttp::Protocol::h1;

      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      auto [ec, request2] = co_await session.submit(url.set_path("echo"), {});
      if (limited)
      {
         EXPECT_EQ(ec, errors::would_block);
         EXPECT_FALSE(request2);
      }
      else
         EXPECT_FALSE(ec) << what(ec);

      check(co_await request1.write_eof(asio::buffer(body1)));
      if (limited)
         request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer(body2)));

      auto response1 = check(co_await request1.get_response());
      EXPECT_EQ(co_await read(response1), body1);
      auto response2 = check(co_await request2.get_response());
      EXPECT_EQ(co_await read(response2), body2);
   };
}

TEST_P(ClientAsync, WHEN_many_requests_are_made_THEN_all_are_answered_in_order)
{
   clientSession = [this](Session session) -> Task<void> {
      std::vector<client::Request> requests;
      for (size_t i = 0; i < 10; ++i)
      {
         requests.push_back(check(co_await session.submit(url.set_path("echo"), {})));
         check(co_await requests.back().write_eof(asio::buffer(std::format("request #{}", i))));
      }

      for (size_t i = 0; i < requests.size(); ++i)
      {
         auto response = check(co_await requests[i].get_response());
         EXPECT_EQ(co_await read(response), std::format("request #{}", i));
      }
   };
}

//
// Likewise, getting a response has to wait until the responses to all earlier requests have been
// read -- otherwise, it would read one of those.
//
TEST_P(ClientAsync, WHEN_getting_response_before_previous_is_read_THEN_reports_would_block)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP(); // requests are multiplexed, nothing to wait for

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer(body1)));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer(body2)));

      auto [ec, response2] = co_await request2.get_response();
      EXPECT_EQ(ec, errors::would_block) << "response #1 not requested yet";

      auto response1 = check(co_await request1.get_response());
      std::tie(ec, response2) = co_await request2.get_response();
      EXPECT_EQ(ec, errors::would_block) << "response #1 not read yet";

      EXPECT_EQ(co_await read(response1), body1);
      response2 = check(co_await request2.get_response());
      EXPECT_EQ(co_await read(response2), body2);
   };
}

//
// A request without a body -- "Content-Length: 0" -- is complete as soon as it has been submitted,
// so the next one can follow right away. Its body has been ended implicitly: ending it again is a
// no-op, and data has nowhere to go.
//
TEST_P(ClientAsync, WHEN_request_has_no_body_THEN_it_is_complete_after_submit)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP(); // requests are multiplexed, nothing to wait for

   clientSession = [this](Session session) -> Task<void> {
      auto request1 =
         check(co_await session.submit(url.set_path("echo"), fields({{"Content-Length", 0}})));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer(body2)));

      auto [ec] = co_await request1.write_eof();
      EXPECT_FALSE(ec) << what(ec);
      std::tie(ec) = co_await request1.write(asio::buffer(body1));
      EXPECT_EQ(ec, errc::broken_pipe) << what(ec);

      auto response1 = check(co_await request1.get_response());
      EXPECT_EQ(co_await read(response1), "");
      auto response2 = check(co_await request2.get_response());
      EXPECT_EQ(co_await read(response2), body2);
   };
}

//
// A request with a body is complete only once async_write_eof() has succeeded, even if all of its
// 'Content-Length' has been written already.
//
TEST_P(ClientAsync, WHEN_content_length_is_written_without_eof_THEN_request_is_not_complete)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP(); // requests are multiplexed, nothing to wait for

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(
         co_await session.submit(url.set_path("echo"), fields({{"Content-Length", body1.size()}})));
      check(co_await request1.write(asio::buffer(body1)));

      auto [ec, request2] = co_await session.submit(url.set_path("echo"), {});
      EXPECT_EQ(ec, errors::would_block);

      check(co_await request1.write_eof());
      request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer(body2)));

      auto response1 = check(co_await request1.get_response());
      EXPECT_EQ(co_await read(response1), body1);
      auto response2 = check(co_await request2.get_response());
      EXPECT_EQ(co_await read(response2), body2);
   };
}

//
// A request that goes away before it is complete leaves the connection in the middle of a message:
// nothing can be sent after it any more.
//
TEST_P(ClientAsync, WHEN_incomplete_request_is_released_THEN_later_requests_report_error)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP(); // requests are multiplexed, and independent of each other

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write(asio::buffer(body1)));
      request1.reset();

      auto [ec, request2] = co_await session.submit(url.set_path("echo"), {});
      EXPECT_EQ(ec, errors::connection_aborted);
   };
}

//
// A request that has been sent, but goes away without asking for its response, leaves that
// response unread on the connection -- and with it, all responses after it.
//
TEST_P(ClientAsync,
       WHEN_request_is_released_without_getting_response_THEN_later_responses_report_error)
{
   if (GetParam() != anyhttp::Protocol::h1)
      GTEST_SKIP(); // requests are multiplexed, and independent of each other

   clientSession = [this](Session session) -> Task<void> {
      auto request1 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request1.write_eof(asio::buffer(body1)));
      auto request2 = check(co_await session.submit(url.set_path("echo"), {}));
      check(co_await request2.write_eof(asio::buffer(body2)));
      request1.reset();

      auto [ec, response] = co_await request2.get_response();
      EXPECT_EQ(ec, errors::connection_aborted);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, EatRequest)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("eat_request"), {}));
      co_await generate(request, 1024);
      auto response = check(co_await request.get_response());
      auto received = co_await drain(response);
      EXPECT_EQ(received, 0);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Dump)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(
         url.set_path("dump space").set_params({{"blah", "white space"}, {"x", "y"}}), {}));
      co_await send_eof(request);
      auto response = check(co_await request.get_response());
      auto dump = co_await read(response);
      EXPECT_THAT(dump, HasSubstr("method: POST"));
      EXPECT_THAT(dump, HasSubstr("path: /dump space"));
      EXPECT_THAT(dump, HasSubstr("  blah=white space"));
   };
}

// =================================================================================================
