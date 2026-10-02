#include "test_fixtures.hpp"

#if ANYHTTP_COROSIO
#include <boost/capy/ex/run.hpp>
#endif

#include <array>
#include <print>
#include <ranges>

using namespace testing;

// =================================================================================================

//
// Backpressure, cancellation and connection loss, on top of the ClientAsync fixture.
//
class ClientAsyncCancellation : public ClientAsync
{
};

INSTANTIATE_TEST_SUITE_P(ClientAsyncCancellation, ClientAsyncCancellation, ValuesIn(protocols()),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsyncCancellation, Backpressure)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());
      auto sender = send(request, rv::iota(uint8_t(0)));
      co_await when_either(std::move(sender), sleep(2s));
      // FIXME: count bytes sent, just like asio::async_write() does
      // FIXME: or even use asio::async_write() on top of a async_write_some() implementation

      //
      // Now that the flow control window is 0, we can't even send an EOF any more -- except over
      // QUIC, where whether the FIN slips out without credit depends on flow control timing, so
      // only assert that for the stream protocols.
      //
      auto rc = co_await when_either(send_eof(request), sleep(100ms));
      if (GetParam() != anyhttp::Protocol::h3)
         EXPECT_EQ(rc.index(), 1);

      // So instead, we start doing this in background, to be resumed as soon as the window reopens.
      launch(context.get_executor(), send_eof(request)); // FIXME: join

      std::println("receiving....");
      error_code ec;
      auto received = co_await try_receive(response, ec);
      std::println("receiving... done, got {} bytes ({})", received, ec.message());
      EXPECT_GT(received, 0);
      // EXPECT_EQ(received, sent);
      // FIXME: we should be able to receive the remainders that already have been buffered
      // FIXME: in the end, this must be the same as the the bytes sent above
   };
}

//
// Cancellation of a large buffer with Content-Length.
//
// Any short write of a body with known content length should result in a 'partial message' error.
//
// FIXME: As of nghttp2 version 1.67, the partial message results in a GOAWAY, so that only one
//        request can be made. The following request should throw an exception.
//
TEST_P(ClientAsyncCancellation, CancellationContentLength)
{
   clientSession = [this](Session session) -> Task<void> {
      const size_t length = 50_m;
      const std::vector<char> buffer(length);
      for (size_t i = 0; i <= 20; ++i)
      {
         if (!session)
            session = check(co_await client->connect());

         Fields fields;
         fields.set("content-length", std::to_string(length));
         auto request = check(co_await session.submit(url.set_path("echo"), fields));
         auto response = check(co_await request.get_response());

         //
         // This is a single large buffer and will be serialized as a single chunk. When writing
         // gets cancelled, there is no way to recover gracefully.
         //
         auto sender = send_and_force_eof(request, std::string_view(buffer));

         error_code ec;
         auto received =
            co_await when_both(when_either(std::move(sender), yield(i)), try_receive(response, ec));
         std::println("received {} bytes (\x1b[1;31m{}\x1b[0m, yielded {})", std::get<1>(received),
                      ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, errors::partial_message);

         session.reset();
      }
   };
}

//
// Cancellation of sending a single, large buffer without Content-Length.
//
// HTTP/1.1: As always when not providing Content-Length, the data is chunked. When sending data
//           as a single, large buffer, this will result in a single, large chunk of same size.
//           If sending that chunk is interrupted, there is no way to recover. The sender will
//           close the connection in this situation.
//
// HTTP/2: Cancelling a large buffer without Content-Length will look to the server just like a
//         short buffer. No error is raised. FIXME: we could try to support cancellation here
//         by closing the stream without sending an EOF. But that would also stop the receiving
//         direction.
//
TEST_P(ClientAsyncCancellation, Cancellation)
{
   clientSession = [this](Session session) -> Task<void> {
      const size_t length = 50_m;
      const std::vector<char> buffer(length, 'a');
      for (size_t i = 0; i <= 20; ++i)
      {
         auto request = check(co_await session.submit(url.set_path("echo"), {}));
         auto response = check(co_await request.get_response());
         auto sender = send_and_drop(std::move(request), std::string_view(buffer));

         error_code ec;
         auto received =
            co_await when_both(when_either(std::move(sender), yield(i)), try_receive(response, ec));
         std::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, errors::partial_message);

         // HTTP/1.1 needs to reconnect here
         // HTTP/2 can handle this without reconnect -- only the stream is cancelled
         if (GetParam() == anyhttp::Protocol::h1)
         {
            session.reset();
            session = check(co_await client->connect());
         }
      }
   };
}

//
// Cancellation of sending a large amount of data that is split into many smaller chunks.
//
// This should work with any protocol, without error. As we don't give a Content-Length in advance,
// cancelling the upload should not be terminal. BUT: cancellation of a parallel group seems to
// do 'terminal' cancellation...
//
// TODO: Aside using operator||, when manually setting up a parallel group, it is possible to
//       specify the cancellation type that should be used.
//
// TODO: If an operation supports "partial" as well, it is free to cancel like that even when
//       requested to do terminal "cancellation". Cancellation types are backward compatible this
//       way.
//
TEST_P(ClientAsyncCancellation, CancellationRange)
{
   clientSession = [this](Session session) -> Task<void> {
      for (size_t i = 6; i <= 6; ++i)
      {
         co_await yield();
         auto request = check(co_await session.submit(url.set_path("echo"), {}));
         auto response = check(co_await request.get_response());
         // auto sender = send_and_force_eof(request, rv::iota(uint8_t(0)));
         auto sender = send_and_drop(std::move(request), rv::iota(uint8_t(0)));

         error_code ec;
         auto received =
            co_await when_both(when_either(std::move(sender), yield(i)), try_receive(response, ec));
         std::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_EQ(ec, errors::partial_message);
         check(co_await client->connect());
      }
   };
}

TEST_P(ClientAsyncCancellation, PerOperationCancellation)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      std::array<uint8_t, 1024> buffer;
#if ANYHTTP_COROSIO
      // a stop request, which is all that capy has
      std::stop_source stop;
      Timer timer(context.get_executor());
      timer.arm(110ms, [&stop] { stop.request_stop(); });
      auto [ec, n] =
         co_await boost::capy::run(stop.get_token())(response.read_some(asio::buffer(buffer)));
#else
      asio::cancellation_signal cancel;
      asio::steady_timer timer(co_await asio::this_coro::executor, 110ms);
      timer.async_wait([&cancel](const boost::system::error_code&) { //
         cancel.emit(asio::cancellation_type::terminal);
      });

      auto token = asio::bind_cancellation_slot(cancel.slot(), as_tuple);
      auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), std::move(token));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
   };
}

TEST_P(ClientAsyncCancellation, CancelAfter)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request =
         check(co_await session.submit(url.set_path("echo").set_params({{"delay", "1000"}}), {}));
#if ANYHTTP_COROSIO
      auto [ec, response] = co_await stop_after(250ms, request.get_response());
#else
      auto [ec, response] = co_await request.async_get_response(cancel_after(250ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);

#if ANYHTTP_COROSIO
      std::tie(ec, response) = co_await stop_after(0ms, request.get_response());
#else
      std::tie(ec, response) = co_await request.async_get_response(cancel_after(0ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);

      std::tie(ec, response) = co_await request.get_response();
      EXPECT_FALSE(ec);

      constexpr auto msg = "Hello, Client!"sv;
      check(co_await request.write_eof(asio::buffer(msg)));
      EXPECT_EQ(co_await read(response), msg);
   };
}

//
// cancel_after on async_read_some(): the client reads from an echo that has nothing to echo yet, so
// the read can only end by the timer. Nothing about Reader provides an executor for that timer
// other than what the token asks of the initiation.
//
TEST_P(ClientAsyncCancellation, WHEN_client_read_some_with_cancel_after_THEN_cancelled)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      std::array<uint8_t, 1024> buffer;
#if ANYHTTP_COROSIO
      auto [ec, n] = co_await stop_after(100ms, response.read_some(asio::buffer(buffer)));
#else
      auto [ec, n] =
         co_await response.async_read_some(asio::buffer(buffer), cancel_after(100ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
      EXPECT_EQ(n, 0u);
   };
}

//
// Same on the server side: the client never sends a body, so the request read hangs until the
// timer cancels it.
//
TEST_P(ClientAsyncCancellation, WHEN_server_read_some_with_cancel_after_THEN_cancelled)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      std::array<uint8_t, 1024> buffer;
#if ANYHTTP_COROSIO
      auto [ec, n] = co_await stop_after(100ms, request.read_some(asio::buffer(buffer)));
#else
      auto [ec, n] =
         co_await request.async_read_some(asio::buffer(buffer), cancel_after(100ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
      EXPECT_EQ(n, 0u);
      check(co_await response.submit(200, {}));
      check(co_await response.write_eof());
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url, {}));
      auto response = check(co_await request.get_response());
      co_await drain(response);
   };
}

//
// A read that completes before its deadline is not affected by cancel_after: the timer is
// cancelled with it and nothing fires later on.
//
TEST_P(ClientAsyncCancellation, WHEN_read_some_completes_before_cancel_after_THEN_ok)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      constexpr auto msg = "Hello, Server!"sv;
      check(co_await request.write_eof(asio::buffer(msg)));

      std::array<char, 1024> buffer;
#if ANYHTTP_COROSIO
      auto [ec, n] = co_await stop_after(10s, response.read_some(asio::buffer(buffer)));
#else
      auto [ec, n] =
         co_await response.async_read_some(asio::buffer(buffer), cancel_after(10s, as_tuple));
#endif
      EXPECT_FALSE(ec);
      EXPECT_EQ(std::string_view(buffer.data(), n), msg.substr(0, n));
      EXPECT_GT(n, 0u);

      co_await drain(response);
   };
}

//
// cancel_after on async_write(): far more than the send window, with nobody reading the echo, so
// the write cannot complete before the timer fires.
//
TEST_P(ClientAsyncCancellation, WHEN_client_write_with_cancel_after_THEN_cancelled)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      const std::vector<uint8_t> body(8_m, 'x');
#if ANYHTTP_COROSIO
      auto [ec] = co_await stop_after(100ms, request.write(asio::buffer(body)));
#else
      auto [ec] = co_await request.async_write(asio::buffer(body), cancel_after(100ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
   };
}

TEST_P(ClientAsyncCancellation, WHEN_send_more_than_content_length_THEN_connection_is_reset)
{
   clientSession = [this](Session session) -> Task<void> {
      Fields fields;
      fields.set("content-length", "1024");
      auto request = check(co_await session.submit(url.set_path("eat_request"), fields));
      auto response = check(co_await request.get_response());
      co_await drain(response);

      auto ep = co_await caught(send(request, rv::iota(uint8_t{0})));

      //
      // Which of the two the write reports is a matter of how far the kernel has gotten with the
      // peer's RST by the time we get to write again -- the first write after it fails with
      // ECONNRESET, any later one with EPIPE. Single-threaded we reliably hit the former, with
      // more than one thread the latter; both mean the same thing here.
      //
      EXPECT_THAT(code(ep), AnyOf(errc::connection_reset, errc::broken_pipe));
   };
}

// =================================================================================================

TEST_P(ClientAsyncCancellation, ClientDropRequest)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());
   };
}

// =================================================================================================

TEST_P(ClientAsyncCancellation, ResetServerDuringRequest)
{
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());

      //
      // The upload goes on while the server is reset under it. It is caught(), so that its failure
      // does not cancel the reset, which is what when_both() does with a task that throws.
      //
      auto reset_server = [this]() -> Task<void> {
         std::println(
            "=============================================================================");
         for (size_t i = 0; i < 10; ++i)
         {
            std::println("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -",
                         i);
            co_await yield();
         }

         std::println(
            "=============================================================================");
         server.reset();

         for (size_t i = 0; i < 10; ++i)
         {
            std::println("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -",
                         i);
            co_await yield();
         }
      };
      std::ignore = co_await when_both(caught(send(request, rv::iota(uint8_t(0)))), reset_server());

      error_code ec;
      auto received = co_await try_receive(response, ec);
      loge("received: {} ({} bytes)", ec.message(), received);
   };
}

TEST_P(ClientAsyncCancellation, DISABLED_SpawnAndForget)
{
   if (GetParam() == anyhttp::Protocol::h1)
      GTEST_SKIP(); // FIXME: ASAN errors

   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit(url.set_path("echo"), {}));
      auto response = check(co_await request.get_response());
      co_await yield();

      std::println("- - spawning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - ");
      launch(context.get_executor(), [](client::Request request) -> Task<void> { //
         std::println("- - SPAWNED - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await yield(5);
         std::println("- - SPAWNED, sending  - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await send(request, rv::iota(uint8_t(0)));
      }(std::move(request)));
   };
}

// =================================================================================================
