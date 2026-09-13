#include "test_fixtures.hpp"

#include <pthread.h>

#include <array>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <span>

// =================================================================================================

INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                           anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_post_data_THEN_receive_echo)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      size_t bytes = 1024;
      auto count = co_await (generate(request, bytes) && count_response(request));
      EXPECT_EQ(bytes, count);
   };
}

TEST_P(ClientAsync, WHEN_post_without_path_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path(""), {});
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("unknown"), {});
      co_await generate(request, 1_m);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 404);
      auto received = co_await drain(response);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("discard"), {});
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_delayed_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      co_await generate(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_with_body_delayed_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto executor = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      auto [ep] = co_await co_spawn(executor, send(request, rv::iota(uint8_t{0})), as_tuple);
      EXPECT_TRUE(ep);
   };
}

TEST_P(ClientAsync, WHEN_invalid_port_in_host_header_THEN_reports_error)
{
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("Host", "host:12345x");
      auto request = co_await session.async_submit(url.set_path("echo"), fields);
      auto response = co_await (send_eof(request) && count_response(request));
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_called_twice_THEN_reports_error)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::system::errc::success);
      std::tie(ec, response) = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::system::errc::connection_already_in_progress);
      EXPECT_EQ(ec, asio::error::basic_errors::already_started);
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_detached_THEN_does_not_crash)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP();

   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"));
      request.async_get_response(detached);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_while_writing_THEN_connection_is_reset)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await sleep(150ms);
      request.reset();
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      auto executor = co_await this_coro::executor;
      auto [ec] = co_await co_spawn(executor, send(request, rv::iota(uint8_t(0))), as_tuple);
      EXPECT_EQ(code(ec), boost::system::errc::connection_reset);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_and_response_THEN_completes_anyway)
{
   // if (GetParam() == anyhttp::Protocol::http11)
   //    GTEST_SKIP(); // FIXME: timeout

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      auto [ec, _] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::beast::http::error::end_of_stream);
      // EXPECT_EQ(ec, std::errc::connection_reset);
   };
}

TEST_P(ClientAsync, WHEN_client_cancels_write_THEN_can_resume)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects

   test = [this](Session session) -> awaitable<void>
   {
      co_await this_coro::throw_if_cancelled(false);
      auto executor = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto response = co_await request.async_get_response();

      // send as much data as possible within 1s, should run into backpressure
      auto [ep] = co_await co_spawn(executor, send(request, rv::iota(uint8_t(0))),
                                    cancel_after(1s, as_tuple));
      EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);

      if (GetParam() == anyhttp::Protocol::h3)
      {
         //
         // QUIC: whether the FIN can slip out while the send window is closed depends on flow
         // control timing, so don't assert either way here. What matters is that ending the
         // upload and draining the response together complete the exchange.
         //
         auto received = co_await (send_eof(request) && drain(response));
         EXPECT_GT(received, 0);
      }
      else
      {
         // now, with a closed window, we cannot even end the upload
         std::tie(ep) = co_await co_spawn(executor, send_eof(request), cancel_after(1ms, as_tuple));
         EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);

         // as we have no control over when the send window is re-opened, wait for it in parallel
         auto received = co_await (send_eof(request) && drain(response));
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

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::uniform_int_distribution<> dist(0, 10);
      constexpr auto msg = "Hello, Client!"sv;
      co_await yield(dist(gen));
      Fields fields;
      fields.set("Content-Length", std::to_string(msg.size()));
      co_await response.async_submit(200, fields);
      co_await yield(dist(gen));
      co_await response.async_write(asio::buffer(msg));
      co_await yield(dist(gen));
      co_await response.async_write_eof();
      co_await yield(dist(gen));
      std::array<uint8_t, 16> data;
      co_await request.async_read_some(asio::buffer(data), as_tuple);
   };
   test = [this](Session session) -> awaitable<void>
   {
      std::uniform_int_distribution<> dist(0, 10);
      for (size_t i = 0; i < 100; ++i)
      {
         std::println(
            "=== {} =========================================================================", i);
         co_await yield(dist(gen));
         Fields fields;
         if (GetParam() == anyhttp::Protocol::http11)
            fields.set("Connection", "Keep-Alive");
         fields.set("Content-Length", "0");
         auto request = co_await session.async_submit(url, fields);
         co_await yield(dist(gen));
         co_await request.async_write_eof();
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
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await drain(request);
      co_await response.async_submit(200, fields({{"Content-Length", hello.size()}}));
      co_await response.async_write_eof(asio::buffer(hello));
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();

      std::string body;
      std::array<char, 4> buffer; // small on purpose: several reads before the end
      for (;;)
      {
         auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), as_tuple);
         if (ec)
         {
            EXPECT_EQ(ec, asio::error::eof);
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
      auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), as_tuple);
      EXPECT_EQ(ec, asio::error::eof);

      // ... but a zero-length read is not a read, and reports nothing
      std::array<char, 0> empty;
      std::tie(ec, n) = co_await response.async_read_some(asio::buffer(empty), as_tuple);
      EXPECT_FALSE(ec);
      EXPECT_EQ(n, 0u);
   };
}

//
// An empty async_write() no longer ends a body -- async_write_eof() does, and nothing else. So a
// message with an empty write in the middle of it still carries everything written after that.
//
TEST_P(ClientAsync, WHEN_empty_buffer_is_written_THEN_body_stays_open)
{
   static const auto tail = "still here"sv;
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      EXPECT_EQ(co_await drain(request), 0u);
      co_await response.async_submit(200, {});
      co_await response.async_write({}); // writes nothing, leaves the body open
      co_await response.async_write_eof(asio::buffer(tail));
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({}); // likewise: the request body stays open
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
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
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await drain(request);
      co_await response.async_submit(200, fields({{"Content-Length", hello.size()}}));
      co_await response.async_write_eof(asio::buffer(hello));

      auto [ec] = co_await response.async_write_eof(as_tuple);
      EXPECT_FALSE(ec);

      std::tie(ec) = co_await response.async_write({}, as_tuple);
      EXPECT_FALSE(ec);

      std::tie(ec) = co_await response.async_write(asio::buffer(hello), as_tuple);
      EXPECT_EQ(ec, boost::system::errc::broken_pipe);

      std::tie(ec) = co_await response.async_write_eof(asio::buffer(hello), as_tuple);
      EXPECT_EQ(ec, boost::system::errc::broken_pipe);
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      EXPECT_EQ(co_await read(response), hello);
   };
}

TEST_P(ClientAsync, HelloWorld)
{
   static const auto hello = "Hello, World!"sv;
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      co_await response.async_write_eof(asio::buffer(hello));
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      auto body = co_await read(response);
      EXPECT_EQ(body, hello);
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
   static const std::vector<uint8_t> body = []
   {
      std::vector<uint8_t> data(256_k);
      std::ranges::generate(data, [i = uint8_t(0)]() mutable { return i++; });
      return data;
   }();

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      // drain the request -- HTTP/1.1 closes the connection on an unfinished parser
      co_await drain(request);

      co_await response.async_submit(200, fields({{"Content-Length", body.size()}}));
      co_await response.async_write_eof(asio::buffer(body));
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      EXPECT_EQ(co_await count_response(request), body.size());
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

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await drain(request);
      co_await response.async_submit(200, {});

      //
      // Far more than the peer's receive window, and the client below doesn't read a byte until
      // this is over, so the write is guaranteed to still be in progress when it is cancelled.
      //
      auto [ec] = co_await response.async_write_eof(asio::buffer(body),
                                                    cancel_after(50ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();

      // leave the body untouched until the cancellation above has hit, see the sibling testcase
      asio::steady_timer timer(co_await this_coro::executor, 150ms);
      co_await timer.async_wait(deferred);

      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      EXPECT_LT(received, body.size());
      EXPECT_EQ(ec, boost::beast::http::error::partial_message);
   };
}

//
// Cancelling an async_write_eof() whose FIN never made it out must not leave the body in limbo:
// the intent to end it is rolled back, and a re-issued async_write_eof() ends the (now shorter)
// body for real -- instead of completing as a no-op while the peer waits forever for the end.
//
TEST_P(ClientAsync, WHEN_client_cancels_write_eof_THEN_can_still_end)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects

   static const std::vector<uint8_t> body(8_m, 'x');

   test = [this](Session session) -> awaitable<void>
   {
      co_await this_coro::throw_if_cancelled(false);
      auto executor = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto response = co_await request.async_get_response();

      // far more than the send window, with nobody reading the echo yet: this cannot complete
      auto write_eof = [&]() -> awaitable<void>
      { co_await request.async_write_eof(asio::buffer(body)); };
      auto [ep] = co_await co_spawn(executor, write_eof(), cancel_after(100ms, as_tuple));
      EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);

      // the FIN never went out with the cancelled write, so the body can still be ended
      auto received = co_await (send_eof(request) && drain(response));
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

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      // drain the request -- HTTP/1.1 closes the connection on an unfinished parser
      co_await drain(request);

      co_await response.async_submit(200, {});

      //
      // Far more than the peer's receive window, and the client below doesn't read a byte until
      // this is over, so the write is guaranteed to still be in progress when it is cancelled.
      //
      auto executor = co_await this_coro::executor;
      auto [ep] = co_await co_spawn(executor, send(response, std::span(body)),
                                    cancel_after(50ms, as_tuple));
      EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();

      //
      // Leave the response body untouched for now: whatever the server manages to send fills up
      // the receive window and stays there, so its write cannot run to completion before the
      // cancellation above hits.
      //
      asio::steady_timer timer(co_await this_coro::executor, 150ms);
      co_await timer.async_wait(deferred);

      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      std::println("received {} of {} bytes ({})", received, body.size(), ec.message());
      EXPECT_LT(received, body.size());
      EXPECT_EQ(ec, boost::beast::http::error::partial_message);
   };
}

// =================================================================================================

TEST_P(ClientAsync, ServerYieldFirst)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await yield(10);
      co_await response.async_submit(200, {});
      co_await yield(10);
      co_await response.async_write_eof();
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write_eof();
      co_await count_response(request);
   };
}

// ----------------------------------------------------------------------------------------------

static std::optional<size_t> stackRemainingBytes()
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
   if (!stackRemainingBytes())
      GTEST_SKIP() << "unable to measure stack on this platform";

   test = [this](Session session) -> awaitable<void>
   {
      auto ex = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      // verify that immediate completion (here, due to an empty buffer) does not cause recursion
      std::array<uint8_t, 0> empty;
      co_await response.async_read_some(asio::buffer(empty));
      auto s0 = stackRemainingBytes().value();
      co_await response.async_read_some(asio::buffer(empty));
      auto s1 = stackRemainingBytes().value();
      EXPECT_EQ(s0, s1);

      // however, ASIO allows us to control this behavior using "immediate executors"
      co_await response.async_read_some(asio::buffer(empty), bind_immediate_executor(ex));
      auto s2 = stackRemainingBytes().value();
      EXPECT_GT(s1, s2);
   };
}

// ----------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Custom)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         auto [ec, n] = co_await request.async_read_some(asio::buffer(buffer), as_tuple);
         if (ec)
         {
            co_await response.async_write_eof();
            co_return;
         }
         co_await response.async_write(asio::buffer(buffer, n));
      }
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {});
      constexpr size_t bytes = 1024;
      auto count = co_await (generate(request, bytes) && count_response(request));
      EXPECT_EQ(bytes, count);
   };
}

TEST_P(ClientAsync, IgnoreRequest)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      co_await response.async_write_eof();
   };
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("content-length", "0");
      auto request = co_await session.async_submit(url, fields);
      auto count = co_await (generate(request, 0) && count_response(request));
      EXPECT_EQ(count, 0);
   };
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {});
      auto res = co_await (generate(request, 0) && try_read_response(request));
      EXPECT_FALSE(res.has_value());
      std::println("ERROR: {}", res.error().message());
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, PostRange)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      // co_await request.async_write(asio::buffer("ping"sv)); // FIXME:
      auto response = co_await request.async_get_response();
      // std::string s(10_m, 'a');
      // auto sender = send(request, std::string_view("blah"));
      // auto sender = send(request, std::string(10_m, 'a'));
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1_m));
      auto received = co_await (std::move(sender) && drain(response));
      loge("received: {}", received);
      EXPECT_EQ(received, 1_m);
   };
}

TEST_P(ClientAsync, PostRangeImmediate)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1_m));
      auto received = co_await (std::move(sender) && count_response(request));
      loge("received: {}", received);
      EXPECT_EQ(received, 1_m);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      constexpr size_t bytes = 1024;
      co_await generate(request, bytes);
      EXPECT_EQ(co_await drain(response), bytes);
   };
}

// -------------------------------------------------------------------------------------------------

//
// HTTP/1.1 supports pipelining in the sense that multiple, full requests can be made before
// the responses are received.
//
// TODO: Any kind of interleaving is not supported. An attempt to issue another request while the
//       previous request is still active should result in an error, immediately.
//
TEST_P(ClientAsync, WHEN_multiple_request_are_made_THEN_responses_are_received_in_order)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request1 = co_await session.async_submit(url.set_path("echo"), {});
      co_await request1.async_write_eof(asio::buffer("Hello, Server #1!"sv));

      auto request2 = co_await session.async_submit(url.set_path("echo"), {});
      co_await request2.async_write_eof(asio::buffer("Hello, Server #2! XYZ"sv));

      auto response1 = co_await request1.async_get_response();
      EXPECT_EQ(co_await drain(response1), 17);

      auto response2 = co_await request2.async_get_response();
      EXPECT_EQ(co_await drain(response2), 21);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, EatRequest)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("eat_request"), {});
      co_await generate(request, 1024);
      auto response = co_await request.async_get_response();
      auto received = co_await drain(response);
      EXPECT_EQ(received, 0);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Dump)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(
         url.set_path("dump space").set_params({{"blah", "white space"}, {"x", "y"}}), {});
      co_await send_eof(request);
      auto response = co_await request.async_get_response();
      auto dump = co_await read(response);
      EXPECT_THAT(dump, testing::HasSubstr("path: /dump space"));
      EXPECT_THAT(dump, testing::HasSubstr("  blah=white space"));
   };
}

// =================================================================================================
