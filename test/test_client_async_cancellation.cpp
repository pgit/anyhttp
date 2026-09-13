#include "test_fixtures.hpp"

#include <array>
#include <print>
#include <ranges>

// =================================================================================================

//
// Backpressure, cancellation and connection loss, on top of the ClientAsync fixture.
//
class ClientAsyncCancellation : public ClientAsync
{
};

INSTANTIATE_TEST_SUITE_P(ClientAsyncCancellation, ClientAsyncCancellation,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                           anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsyncCancellation, Backpressure)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      auto sender = send(request, rv::iota(uint8_t(0)));
      co_await (std::move(sender) || sleep(2s));
      // FIXME: count bytes sent, just like asio::async_write() does
      // FIXME: or even use asio::async_write() on top of a async_write_some() implementation

      //
      // Now that the flow control window is 0, we can't even send an EOF any more -- except over
      // QUIC, where whether the FIN slips out without credit depends on flow control timing, so
      // only assert that for the stream protocols.
      //
      auto rc = co_await (send_eof(request) || sleep(100ms));
      if (GetParam() != anyhttp::Protocol::h3)
         EXPECT_EQ(rc.index(), 1);

      // So instead, we start doing this in background, to be resumed as soon as the window reopens.
      co_spawn(co_await this_coro::executor, send_eof(request), detached); // FIXME: join

      std::println("receiving....");
      boost::system::error_code ec;
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
   test = [this](Session session) -> awaitable<void>
   {
      const size_t length = 50_m;
      const std::vector<char> buffer(length);
      for (size_t i = 0; i <= 20; ++i)
      {
         if (!session)
            session = co_await client->async_connect();

         Fields fields;
         fields.set("content-length", std::to_string(length));
         auto request = co_await session.async_submit(url.set_path("echo"), fields);
         auto response = co_await request.async_get_response();

         //
         // This is a single large buffer and will be serialized as a single chunk. When writing
         // gets cancelled, there is no way to recover gracefully.
         //
         auto sender = sendAndForceEOF(request, std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         std::println("received {} bytes (\x1b[1;31m{}\x1b[0m, yielded {})", std::get<1>(received),
                      ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);

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
   test = [this](Session session) -> awaitable<void>
   {
      const size_t length = 50_m;
      const std::vector<char> buffer(length, 'a');
      for (size_t i = 0; i <= 20; ++i)
      {
         auto request = co_await session.async_submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response();
         auto sender = sendAndDrop(std::move(request), std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         std::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);

         // HTTP/1.1 needs to reconnect here
         // HTTP/2 can handle this without reconnect -- only the stream is cancelled
         if (GetParam() == anyhttp::Protocol::http11)
         {
            session.reset();
            session = co_await client->async_connect();
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
   test = [this](Session session) -> awaitable<void>
   {
      for (size_t i = 6; i <= 6; ++i)
      {
         co_await yield();
         auto request = co_await session.async_submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response();
         // auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)));
         auto sender = sendAndDrop(std::move(request), rv::iota(uint8_t(0)));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         std::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);
         co_await client->async_connect();
      }
   };
}

TEST_P(ClientAsyncCancellation, PerOperationCancellation)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      asio::cancellation_signal cancel;
      asio::steady_timer timer(co_await asio::this_coro::executor, 110ms);
      timer.async_wait([&cancel](const boost::system::error_code& ec) { //
         cancel.emit(asio::cancellation_type::terminal);
      });

      std::array<uint8_t, 1024> buffer;
      auto token = asio::bind_cancellation_slot(cancel.slot(), as_tuple);
      auto [ec, n] = co_await response.async_read_some(asio::buffer(buffer), std::move(token));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
   };
}

TEST_P(ClientAsyncCancellation, CancelAfter)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request =
         co_await session.async_submit(url.set_path("echo").set_params({{"delay", "1000"}}), {});
      auto [ec, response] = co_await request.async_get_response(cancel_after(250ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);

      std::tie(ec, response) = co_await request.async_get_response(cancel_after(0ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);

      std::tie(ec, response) = co_await request.async_get_response(as_tuple);
      EXPECT_FALSE(ec);

      constexpr auto msg = "Hello, Client!"sv;
      co_await request.async_write_eof(asio::buffer(msg));
      EXPECT_EQ(co_await read(response), msg);
   };
}

TEST_P(ClientAsyncCancellation, WHEN_send_more_than_content_length_THEN_connection_is_reset)
{
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("content-length", "1024");
      auto request = co_await session.async_submit(url.set_path("eat_request"), fields);
      auto response = co_await request.async_get_response();
      co_await drain(response);

      auto ex = co_await this_coro::executor;
      auto [ep] = co_await co_spawn(ex, send(request, rv::iota(uint8_t(0))), as_tuple);

      //
      // Which of the two the write reports is a matter of how far the kernel has gotten with the
      // peer's RST by the time we get to write again -- the first write after it fails with
      // ECONNRESET, any later one with EPIPE. Single-threaded we reliably hit the former, with
      // more than one thread the latter; both mean the same thing here.
      //
      EXPECT_THAT(code(ep), testing::AnyOf(boost::system::errc::connection_reset,
                                           boost::system::errc::broken_pipe));
   };
}

// =================================================================================================

TEST_P(ClientAsyncCancellation, ClientDropRequest)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
   };
}

// =================================================================================================

TEST_P(ClientAsyncCancellation, ResetServerDuringRequest)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      //
      // Deliberately NOT use_future(): with more than one thread the client lives on a strand,
      // and blocking that strand in future.get() below would keep the very handlers that
      // complete this send from ever running. asio::experimental::promise starts the coroutine
      // right away, just like use_future, but is awaited instead of waited on.
      //
      auto promise = co_spawn(request.get_executor(), send(request, rv::iota(uint8_t(0))),
                              asio::experimental::use_promise);

      std::println("=============================================================================");
      for (size_t i = 0; i < 10; ++i)
      {
         std::println("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -", i);
         co_await yield();
      }

      std::println("=============================================================================");
      server.reset();

      for (size_t i = 0; i < 10; ++i)
      {
         std::println("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -", i);
         co_await yield();
      }

      auto exception_ptr = co_await std::move(promise)(as_tuple(use_awaitable));

      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      loge("received: {} ({} bytes)", ec.message(), received);
   };
}

TEST_P(ClientAsyncCancellation, DISABLED_SpawnAndForget)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // FIXME: ASAN errors

   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      co_await yield();

      std::println("- - spawning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - ");
      co_spawn(context,
               [request = std::move(request)]() mutable -> awaitable<void>
      { //
         std::println("- - SPAWNED - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await yield(5);
         std::println("- - SPAWNED, sending  - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await send(request, rv::iota(uint8_t(0)));
      }, detached);
   };
}

// =================================================================================================
