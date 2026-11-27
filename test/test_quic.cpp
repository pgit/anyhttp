#include "test_fixtures.hpp"

#include "anyhttp/net.hpp"
#include "anyhttp/quic.hpp"

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <vector>

using namespace testing;

// =================================================================================================

namespace
{

/// Reads \p reader to its end, or to the first error: what it had, and how it ended (success
/// for a clean end).
IoTask<std::string> read_all(Reader& reader)
{
   std::string body;
   std::array<char, 4096> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.read_some(make_buffer(buffer));
      body.append(buffer.data(), n);
      if (ec)
         co_return std::tuple{ec == errors::eof ? error_code{} : ec, std::move(body)};
   }
}

/// \p connection.open_stream(), cancelled after \p timeout the way each runtime cancels: ASIO with
/// cancel_after() on the operation, COROSIO with a stop request.
IoTask<quic::Stream> open_stream_within(quic::Connection& connection,
                                        std::chrono::milliseconds timeout)
{
#if ANYHTTP_COROSIO
   co_return co_await stop_after(timeout, connection.open_stream());
#else
   co_return co_await connection.async_open_stream(cancel_after(timeout, as_tuple));
#endif
}

/// A body of \p size bytes that is not the same all over, so that misplaced bytes show.
std::string pattern(size_t size)
{
   std::string body(size, '\0');
   for (size_t i = 0; i < size; ++i)
      body[i] = static_cast<char>('a' + (i * 7 + i / 1024) % 26);
   return body;
}

/// Accepts streams and echoes each one back, concurrently, until the connection is over.
Task<void> echo_streams(quic::Connection connection)
{
   std::vector<Task<void>> echoes;
   auto echo = [](quic::Stream stream) -> Task<void> {
      auto [ec, body] = co_await read_all(stream);
      EXPECT_FALSE(ec) << ec.message();
      auto [write_ec, written] = co_await stream.write_eof(make_buffer(body));
      std::ignore = write_ec; // the client may be gone before it acknowledges the end
   };
   for (;;)
   {
      auto [ec, stream] = co_await connection.accept_stream();
      if (ec)
         co_return;
      launch(connection.get_executor(), echo(std::move(stream)));
   }
}

/// Sends \p text over a stream of its own, to a server that echoes it, and returns what came back.
Task<std::string> echoed(quic::Connection& connection, std::string text)
{
   auto [ec, stream] = co_await connection.open_stream();
   if (ec)
      throw_error(ec);
   std::ignore = co_await stream.write_eof(make_buffer(text));
   auto [read_ec, body] = co_await read_all(stream);
   if (read_ec)
      throw_error(read_ec);
   co_return body;
}

} // namespace

// =================================================================================================

//
// Plain QUIC streams between the library's own server and client, over the UDP port HTTP/3 is
// on. Parametrized by the write mode: copied in chunks (Staged), or zero-copy, completing on
// acknowledgement.
//
class QuicStreams : public testing::TestWithParam<bool>
{
protected:
   static constexpr std::string_view alpn = "anyhttp-test";

   void SetUp() override
   {
      setup_logging();
      server.emplace(context.get_executor(),
                     server::Config{.listen_address = "127.0.0.2", .port = 0});
      auto config = server_config();
      configure_server(config);
      server->on_quic(config, [this](quic::Connection connection) -> Task<void> {
         if (serverConnection)
            co_await serverConnection(std::move(connection));
      });
   }

   quic::Config server_config() const
   {
      return {.alpn = std::string(alpn), .zero_copy = GetParam()};
   }

   quic::Config client_config() const
   {
      return {
         .alpn = std::string(alpn), .tls_ca_file = "pki/out/root.pem", .zero_copy = GetParam()};
   }

   virtual void configure_server(quic::Config&) {}

   std::string port() const { return std::to_string(server->local_endpoint().port()); }

   Task<quic::Connection> connect(quic::Config config)
   {
      auto [ec, connection] =
         co_await quic::connect(context.get_executor(), "127.0.0.2", port(), std::move(config));
      if (ec)
         throw_error(ec);
      co_return std::move(connection);
   }

   Task<quic::Connection> connect() { return connect(client_config()); }

   /// Runs \p task to completion, then stops the server.
   void run(Task<void> task)
   {
      launch(context.get_executor(), std::move(task), [this](const std::exception_ptr& ep) {
         if (ep)
            ADD_FAILURE() << what(ep);
         server.reset();
      });
      ::run(context);
   }

   IoContext context;
   std::optional<server::Server> server;
   std::function<Task<void>(quic::Connection)> serverConnection;
};

INSTANTIATE_TEST_SUITE_P(QuicStreams, QuicStreams, Values(false, true),
                         [](const auto& info) { return info.param ? "ZeroCopy" : "Staged"; });

// -------------------------------------------------------------------------------------------------

TEST_P(QuicStreams, WHEN_stream_is_echoed_THEN_both_ends_see_the_whole_body)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_EQ(stream.id(), 0);

      auto [write_ec, written] = co_await stream.write(make_buffer("Hello, ", 7));
      EXPECT_FALSE(write_ec) << write_ec.message();
      auto [eof_ec, eof_written] = co_await stream.write_eof(make_buffer("World!", 6));
      EXPECT_FALSE(eof_ec) << eof_ec.message();

      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(body, "Hello, World!");
   }());
}

TEST_P(QuicStreams, WHEN_bodies_are_larger_than_the_windows_THEN_they_arrive_intact)
{
   //
   // Several streams at once, each bigger than its flow control window and all of them together
   // bigger than the connection's: the write queue has to take turns, and the windows have to
   // open up as the reader reads.
   //
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();

      auto exchange = [](quic::Stream stream, std::string body) -> Task<void> {
         auto [write_ec, written] = co_await stream.write_eof(make_buffer(body));
         EXPECT_FALSE(write_ec) << write_ec.message();
         auto [read_ec, echoed] = co_await read_all(stream);
         EXPECT_FALSE(read_ec) << read_ec.message();
         EXPECT_EQ(echoed.size(), body.size());
         EXPECT_TRUE(echoed == body);
      };

      //
      // The writes complete before the echoes are read, so all of a stream's echo is buffered
      // -- up to its window -- until then: read concurrently, by awaiting the exchanges together.
      //
      auto [ec1, first] = co_await connection.open_stream();
      auto [ec2, second] = co_await connection.open_stream();
      co_await when_both(exchange(std::move(first), pattern(1024 * 1024)),
                         exchange(std::move(second), pattern(700 * 1024 + 17)));
   }());
}

TEST_P(QuicStreams, WHEN_one_side_ends_its_body_THEN_the_other_can_still_write)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      auto [read_ec, request] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(request, "question");

      // the client's side is over, ours is not
      for (auto part : {"an", "s", "wer"})
      {
         auto [write_ec, written] = co_await stream.write(make_buffer(std::string_view(part)));
         EXPECT_FALSE(write_ec) << write_ec.message();
      }
      std::ignore = co_await stream.write_eof();
      std::ignore = co_await connection.accept_stream(); // until the client closes
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      auto [eof_ec, eof_written] = co_await stream.write_eof(make_buffer("question", 8));
      EXPECT_FALSE(eof_ec) << eof_ec.message();

      auto [read_ec, response] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(response, "answer");

      // the end is final: data after it is refused, a second end is a no-op
      auto [late_ec, late_written] = co_await stream.write(make_buffer("x", 1));
      EXPECT_EQ(late_ec, errc::broken_pipe);
      auto [again_ec, again_written] = co_await stream.write_eof();
      EXPECT_FALSE(again_ec) << again_ec.message();
   }());
}

TEST_P(QuicStreams, WHEN_peer_resets_its_side_THEN_reads_report_partial_message)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(read_ec, errors::partial_message);

      //
      // Our side of the stream is unaffected. (Not checked: with zero copy, the write completes
      // when the client acknowledges it, and the client may be gone by then.)
      //
      std::ignore = co_await stream.write_eof(make_buffer("still here", 10));
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      auto [write_ec, written] = co_await stream.write(make_buffer("partial", 7));
      EXPECT_FALSE(write_ec) << write_ec.message();
      co_await delay(50ms); // let it arrive, so that the server sees a stream to accept
      stream.reset_stream(42);

      auto [late_ec, late_written] = co_await stream.write(make_buffer("more", 4));
      EXPECT_EQ(late_ec, errc::connection_reset);

      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(body, "still here");
   }());
}

TEST_P(QuicStreams, WHEN_peer_stops_reading_THEN_writes_fail_with_connection_reset)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      stream.stop_sending(7);

      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(read_ec, errors::partial_message);
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();

      //
      // Writes keep succeeding until the STOP_SENDING has arrived, a round trip after the first
      // one has. Give it a few hundred milliseconds' worth of them.
      //
      error_code write_ec;
      for (int i = 0; i < 100 && !write_ec; ++i)
      {
         std::tie(write_ec, std::ignore) = co_await stream.write(make_buffer("data", 4));
         if (!write_ec)
            co_await delay(5ms);
      }
      EXPECT_EQ(write_ec, errc::connection_reset);

      auto [late_ec, late_written] = co_await stream.write(make_buffer("more", 4));
      EXPECT_EQ(late_ec, errc::connection_reset);
   }());
}

TEST_P(QuicStreams, WHEN_writer_is_dropped_before_the_end_THEN_peer_sees_partial_message)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(read_ec, errors::partial_message);
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      auto [write_ec, written] = co_await stream.write(make_buffer("unfinished", 10));
      EXPECT_FALSE(write_ec) << write_ec.message();
      co_await delay(50ms);
      {
         Writer writer = std::move(stream); // only the writing half...
      } // ... goes away here
      co_await delay(100ms);
   }());
}

class QuicStreamsSmallWindow : public QuicStreams
{
protected:
   void configure_server(quic::Config& config) override
   {
      config.max_stream_data = 64 * 1024;
      config.max_streams = 2;
   }
};

INSTANTIATE_TEST_SUITE_P(QuicStreamsSmallWindow, QuicStreamsSmallWindow, Values(false, true),
                         [](const auto& info) { return info.param ? "ZeroCopy" : "Staged"; });

TEST_P(QuicStreamsSmallWindow, WHEN_reader_is_slow_THEN_flow_control_holds_the_writer_back)
{
   static constexpr size_t window = 64 * 1024;
   bool reading = false;

   serverConnection = [&](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      co_await delay(300ms);
      reading = true;
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(body.size(), 8 * window);
      std::ignore = co_await stream.write_eof();
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      auto body = pattern(8 * window);
      auto [write_ec, written] = co_await stream.write_eof(make_buffer(body));
      EXPECT_FALSE(write_ec) << write_ec.message();
      EXPECT_TRUE(reading) << "the write completed before the server started reading";
      std::ignore = co_await read_all(stream);
   }());
}

//
// At the peer's limit, try_open_stream() refuses, and open_stream() waits for a stream to be over
// -- at both ends, which the server's MAX_STREAMS tells: the client has done its part when it has
// read the echo, and does nothing more.
//
TEST_P(QuicStreamsSmallWindow, WHEN_peer_allows_no_more_streams_THEN_try_refuses_and_open_waits)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec1, first] = connection.try_open_stream();
      EXPECT_FALSE(ec1) << ec1.message();
      auto [ec2, second] = connection.try_open_stream();
      EXPECT_FALSE(ec2) << ec2.message();
      auto [ec3, third] = connection.try_open_stream();
      EXPECT_EQ(ec3, errors::would_block);
      EXPECT_FALSE(third);

      std::ignore = co_await first.write_eof(make_buffer("1", 1));
      auto [read_ec, body] = co_await read_all(first);
      EXPECT_EQ(body, "1");

      auto [ec4, fourth] = co_await open_stream_within(connection, 2s);
      EXPECT_FALSE(ec4) << ec4.message();
      EXPECT_EQ(fourth.id(), 8);
   }());
}

// -------------------------------------------------------------------------------------------------

//
// A server that allows one stream at a time, as a DNS over QUIC server may: each open has to wait
// for the stream before it to be over.
//
class QuicStreamsOneStream : public QuicStreams
{
protected:
   void configure_server(quic::Config& config) override { config.max_streams = 1; }
};

INSTANTIATE_TEST_SUITE_P(QuicStreamsOneStream, QuicStreamsOneStream, Values(false, true),
                         [](const auto& info) { return info.param ? "ZeroCopy" : "Staged"; });

TEST_P(QuicStreamsOneStream, WHEN_the_one_stream_is_open_THEN_the_next_open_waits_for_its_end)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, first] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();

      bool opened = false;
      auto open_second = [&]() -> Task<void> {
         auto [open_ec, second] = co_await open_stream_within(connection, 2s);
         opened = true;
         EXPECT_FALSE(open_ec) << open_ec.message();
         EXPECT_EQ(second.id(), 4);
         std::ignore = co_await second.write_eof(make_buffer("2", 1));
         auto [read_ec, body] = co_await read_all(second);
         EXPECT_EQ(body, "2");
      };
      auto finish_first = [&]() -> Task<void> {
         co_await delay(50ms);
         EXPECT_FALSE(opened) << "the second stream opened while the first was open";
         std::ignore = co_await first.write_eof(make_buffer("1", 1));
         auto [read_ec, body] = co_await read_all(first);
         EXPECT_EQ(body, "1");
      };
      co_await when_both(open_second(), finish_first());
      EXPECT_TRUE(opened);
   }());
}

TEST_P(QuicStreamsOneStream, WHEN_several_opens_wait_THEN_they_get_their_streams_in_order)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, first] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();

      std::vector<std::string> order;
      auto echo_on_next = [&](std::string name, std::chrono::milliseconds after) -> Task<void> {
         co_await delay(after); // so that the waits start in this order
         auto [open_ec, stream] = co_await open_stream_within(connection, 2s);
         EXPECT_FALSE(open_ec) << name << ": " << open_ec.message();
         order.push_back(std::format("{} on {}", name, stream.id()));
         std::ignore = co_await stream.write_eof(make_buffer(name));
         auto [read_ec, body] = co_await read_all(stream);
         EXPECT_EQ(body, name);
      };
      auto finish_first = [&]() -> Task<void> {
         co_await delay(50ms);
         std::ignore = co_await first.write_eof(make_buffer("1", 1));
         std::ignore = co_await read_all(first);
      };
      co_await when_both(when_both(echo_on_next("a", 0ms), echo_on_next("b", 10ms)),
                         finish_first());
      EXPECT_THAT(order, ElementsAre("a on 4", "b on 8"));
   }());
}

TEST_P(QuicStreamsOneStream, WHEN_waiting_open_is_cancelled_THEN_it_fails_and_takes_no_stream)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, first] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();

      auto [cancelled_ec, none] = co_await open_stream_within(connection, 50ms);
      EXPECT_EQ(cancelled_ec, errors::canceled);
      EXPECT_FALSE(none);

      std::ignore = co_await first.write_eof(make_buffer("1", 1));
      std::ignore = co_await read_all(first);
      auto [open_ec, second] = co_await open_stream_within(connection, 2s);
      EXPECT_FALSE(open_ec) << open_ec.message();
      EXPECT_EQ(second.id(), 4);
   }());
}

TEST_P(QuicStreamsOneStream, WHEN_connection_closes_THEN_waiting_open_fails)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      co_await delay(50ms);
      connection.close(7);
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, first] = co_await connection.open_stream();
      std::ignore = co_await first.write(make_buffer("hi", 2)); // for the server to accept it

      auto [open_ec, none] = co_await open_stream_within(connection, 2s);
      EXPECT_EQ(open_ec, errors::connection_aborted);
      EXPECT_FALSE(none);
   }());
}

TEST_P(QuicStreamsOneStream, WHEN_connection_is_closed_here_THEN_waiting_open_fails)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, first] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();

      auto open_second = [&]() -> Task<void> {
         auto [open_ec, none] = co_await open_stream_within(connection, 2s);
         EXPECT_EQ(open_ec, errors::connection_aborted);
         EXPECT_FALSE(none);
      };
      auto close_later = [&]() -> Task<void> {
         co_await delay(50ms);
         connection.close();
      };
      co_await when_both(open_second(), close_later());

      auto [late_ec, late] = co_await connection.open_stream();
      EXPECT_EQ(late_ec, errors::connection_aborted);
   }());
}

// -------------------------------------------------------------------------------------------------

TEST_P(QuicStreams, WHEN_server_opens_a_stream_THEN_client_accepts_it)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_EQ(stream.id(), 1); // the first server-initiated bidirectional stream
      std::ignore = co_await stream.write_eof(make_buffer("push", 4));
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(body, "ack");
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(body, "push");
      std::ignore = co_await stream.write_eof(make_buffer("ack", 3));
      co_await delay(50ms);
   }());
}

TEST_P(QuicStreams, WHEN_connection_is_closed_THEN_what_waits_on_it_fails)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      connection.close(99);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_TRUE(read_ec);
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      std::ignore = co_await stream.write(make_buffer("hi", 2));

      auto [accept_ec, accepted] = co_await connection.accept_stream();
      EXPECT_EQ(accept_ec, errors::connection_aborted);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(read_ec, errors::partial_message);

      auto [open_ec, opened] = co_await connection.open_stream();
      EXPECT_EQ(open_ec, errors::connection_aborted);
   }());
}

TEST_P(QuicStreams, WHEN_write_is_cancelled_THEN_the_stream_goes_on_with_a_shorter_body)
{
   static const auto body = pattern(4 * 1024 * 1024);

   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.accept_stream();
      EXPECT_FALSE(ec) << ec.message();
      co_await delay(300ms); // nothing is read, so the big write can't complete before that
      auto [read_ec, received] = co_await read_all(stream);
      if (!read_ec)
      {
         EXPECT_TRUE(received.ends_with("end"));
         received.resize(received.size() - 3);
         EXPECT_LT(received.size(), body.size());
         EXPECT_TRUE(received == body.substr(0, received.size()));
      }
      else
         EXPECT_EQ(read_ec, errors::partial_message);
      std::ignore = co_await stream.write_eof();
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      auto copy = body;
      auto result = co_await when_either(stream.write(make_buffer(copy)), delay(100ms));
      EXPECT_EQ(result.index(), 1) << "the write completed, although nothing was read";
      copy.assign(copy.size(), '!'); // a cancelled write releases the buffer

      //
      // Staged: the stream survives, with what was copied of the body. Zero copy: so it does, if
      // what the stack was pointing into the buffer for has been acknowledged -- which, the
      // stream being blocked by flow control, it usually is. Otherwise the stream had to be
      // reset.
      //
      auto [write_ec, written] = co_await stream.write_eof(make_buffer("end", 3));
      if (GetParam() && write_ec)
         EXPECT_EQ(write_ec, errc::connection_reset);
      else
         EXPECT_FALSE(write_ec) << write_ec.message();
      std::ignore = co_await read_all(stream);
   }());
}

TEST_P(QuicStreams, WHEN_alpn_is_unknown_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      auto config = client_config();
      config.alpn = "nobody-speaks-this";
      auto [ec, connection] =
         co_await quic::connect(context.get_executor(), "127.0.0.2", port(), config);
      EXPECT_TRUE(ec);
      EXPECT_FALSE(connection);
   }());
}

// -------------------------------------------------------------------------------------------------

//
// connect() to endpoints the client has already, with the name that the certificate has to be
// valid for given apart from them: the server is at 127.0.0.2, and "localhost" is one of the
// names of its certificate, "anyhttp.example" is not.
//
TEST_P(QuicStreams, WHEN_connecting_to_endpoints_THEN_the_certificate_is_checked_for_the_name_given)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto [ec, connection] = co_await quic::connect(
         context.get_executor(), {server->local_endpoint()}, "localhost", client_config());
      EXPECT_FALSE(ec) << ec.message();
      if (!connection)
         co_return;

      auto [open_ec, stream] = co_await connection.open_stream();
      std::ignore = co_await stream.write_eof(make_buffer("named", 5));
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(body, "named");
   }());
}

TEST_P(QuicStreams, WHEN_the_name_given_is_not_the_certificates_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      auto [ec, connection] = co_await quic::connect(
         context.get_executor(), {server->local_endpoint()}, "anyhttp.example", client_config());
      EXPECT_TRUE(ec);
      EXPECT_FALSE(connection);
   }());
}

TEST_P(QuicStreams, WHEN_no_name_is_given_THEN_the_chain_is_still_checked)
{
   run([&]() -> Task<void> {
      auto [ec, connection] = co_await quic::connect(
         context.get_executor(), {server->local_endpoint()}, "", client_config());
      EXPECT_FALSE(ec) << ec.message();

      // the default trust store, which does not have the test PKI's root
      auto config = client_config();
      config.tls_ca_file.clear();
      auto [untrusted_ec, untrusted] =
         co_await quic::connect(context.get_executor(), {server->local_endpoint()}, "", config);
      EXPECT_TRUE(untrusted_ec);
      EXPECT_FALSE(untrusted);
   }());
}

TEST_P(QuicStreams, WHEN_verification_is_off_THEN_any_certificate_is_taken)
{
   run([&]() -> Task<void> {
      // neither trusted nor for this name
      auto config = client_config();
      config.tls_ca_file.clear();
      config.tls_verify = false;
      auto [ec, connection] = co_await quic::connect(
         context.get_executor(), {server->local_endpoint()}, "anyhttp.example", config);
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_TRUE(connection);
   }());
}

// -------------------------------------------------------------------------------------------------

//
// Where the client sends from. The server is at 127.0.0.2, which the system sends to from
// 127.0.0.1 unless told otherwise; 127.0.0.3 is on loopback, too, on Linux.
//
TEST_P(QuicStreams, WHEN_a_local_address_is_given_THEN_the_server_sees_the_connection_come_from_it)
{
   UdpEndpoint seen;
   serverConnection = [&](quic::Connection connection) -> Task<void> {
      seen = connection.remote_endpoint();
      co_await echo_streams(std::move(connection));
   };
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_address = "127.0.0.3";
      auto connection = co_await connect(config);
      EXPECT_EQ(connection.remote_endpoint(),
                UdpEndpoint(io::make_address("127.0.0.2"), server->local_endpoint().port()));

      // once it is back, the server has had the connection
      auto body = co_await echoed(connection, "from 127.0.0.3");
      EXPECT_EQ(body, "from 127.0.0.3");
   }());
   EXPECT_EQ(seen.address(), io::make_address("127.0.0.3"));
   EXPECT_NE(seen.port(), 0);
}

TEST_P(QuicStreams, WHEN_a_local_port_is_given_THEN_the_server_sees_the_connection_come_from_it)
{
   // a port that is free: what the system picked for a socket that is gone again
   auto probe = io::make_udp_socket(context.get_executor());
   const UdpEndpoint any_port{io::make_address("127.0.0.3"), 0};
   ASSERT_FALSE(io::open(probe, any_port));
   ASSERT_FALSE(io::bind(probe, any_port));
   const auto local = io::local_endpoint(probe);
   io::close(probe);

   UdpEndpoint seen;
   serverConnection = [&](quic::Connection connection) -> Task<void> {
      seen = connection.remote_endpoint();
      co_await echo_streams(std::move(connection));
   };
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_address = "127.0.0.3";
      config.local_port = local.port();
      auto connection = co_await connect(config);
      auto body = co_await echoed(connection, "from a port of its own");
      EXPECT_EQ(body, "from a port of its own");
   }());
   EXPECT_EQ(seen, local);
}

TEST_P(QuicStreams, WHEN_the_local_address_is_not_this_hosts_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_address = "192.0.2.1"; // TEST-NET-1
      auto [ec, connection] =
         co_await quic::connect(context.get_executor(), "127.0.0.2", port(), config);
      EXPECT_EQ(ec, errc::address_not_available) << ec.message();
      EXPECT_FALSE(connection);
   }());
}

TEST_P(QuicStreams, WHEN_the_local_address_is_of_the_other_family_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_address = "::1";
      auto [ec, connection] =
         co_await quic::connect(context.get_executor(), "127.0.0.2", port(), config);
      EXPECT_EQ(ec, errc::address_family_not_supported) << ec.message();
      EXPECT_FALSE(connection);
   }());
}

TEST_P(QuicStreams, WHEN_the_local_address_is_not_an_address_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_address = "localhost"; // a name
      auto [ec, connection] =
         co_await quic::connect(context.get_executor(), "127.0.0.2", port(), config);
      EXPECT_EQ(ec, errc::invalid_argument) << ec.message();
      EXPECT_FALSE(connection);
   }());
}

// "*", either family, with a port: the port is bound, with the unspecified address of the server's.
TEST_P(QuicStreams, WHEN_only_a_local_port_is_given_THEN_the_server_sees_the_connection_from_it)
{
   auto probe = io::make_udp_socket(context.get_executor());
   const UdpEndpoint any_port{io::make_address("127.0.0.1"), 0};
   ASSERT_FALSE(io::open(probe, any_port));
   ASSERT_FALSE(io::bind(probe, any_port));
   const auto port = io::local_endpoint(probe).port();
   io::close(probe);

   UdpEndpoint seen;
   serverConnection = [&](quic::Connection connection) -> Task<void> {
      seen = connection.remote_endpoint();
      co_await echo_streams(std::move(connection));
   };
   run([&]() -> Task<void> {
      auto config = client_config();
      config.local_port = port;
      auto connection = co_await connect(config);
      auto body = co_await echoed(connection, "from a port alone");
      EXPECT_EQ(body, "from a port alone");
   }());
   EXPECT_EQ(seen.port(), port);
}

TEST_P(QuicStreams, WHEN_a_device_is_given_THEN_the_connection_goes_through_it)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto config = client_config();
      config.device = "lo";
      auto connection = co_await connect(config);
      auto body = co_await echoed(connection, "through lo");
      EXPECT_EQ(body, "through lo");
   }());
}

TEST_P(QuicStreams, WHEN_there_is_no_such_device_THEN_connect_fails)
{
   run([&]() -> Task<void> {
      //
      // No interface has any of these names. But the kernel takes a name up to its first NUL,
      // which would make the second "lo", and cuts it short to what a name can be (15 characters)
      // without telling, which the third would be.
      //
      for (const auto& device :
           {std::string("nonexistent0"), std::string("lo\0x", 4), std::string(16, 'x')})
      {
         auto config = client_config();
         config.device = device;
         auto [ec, connection] =
            co_await quic::connect(context.get_executor(), "127.0.0.2", port(), config);
         EXPECT_EQ(ec, errc::no_such_device) << device << ": " << ec.message();
         EXPECT_FALSE(connection);
      }
   }());
}

TEST_P(QuicStreams, WHEN_http3_is_on_the_same_port_THEN_both_work)
{
   serverConnection = echo_streams;
   server->on_request([](server::Request request, server::Response response) -> Task<void> {
      co_await not_found(std::move(request), std::move(response));
   });

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = co_await connection.open_stream();
      std::ignore = co_await stream.write_eof(make_buffer("plain", 5));
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(body, "plain");

      boost::urls::url url{"https://127.0.0.2/"};
      url.set_port(port());
      client::Client client(
         context.get_executor(),
         client::Config{.url = url, .protocol = Protocol::h3, .tls_ca_file = "pki/out/root.pem"});
      auto [connect_ec, session] = co_await client.connect();
      EXPECT_FALSE(connect_ec) << connect_ec.message();
      auto [get_ec, message] = co_await session.get(url, {});
      EXPECT_FALSE(get_ec) << get_ec.message();
      EXPECT_EQ(message.status_int(), 404);
   }());
}

// =================================================================================================

#if ANYHTTP_ASIO

//
// The completion token spelling, which only ASIO has.
//
TEST_P(QuicStreams, WHEN_using_completion_tokens_THEN_the_same_works)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = co_await connection.async_accept_stream(as_tuple(use_awaitable));
      EXPECT_FALSE(ec) << ec.message();
      auto [read_ec, body] = co_await read_all(stream);
      co_await stream.async_write_eof(boost::asio::buffer(body), use_awaitable);
      std::ignore = co_await connection.async_accept_stream(as_tuple(use_awaitable));
   };

   run([&]() -> Task<void> {
      auto connection = co_await quic::async_connect(context.get_executor(), "127.0.0.2", port(),
                                                     client_config(), use_awaitable);
      auto stream = co_await connection.async_open_stream(use_awaitable);
      co_await stream.async_write_eof(boost::asio::buffer("token", 5), use_awaitable);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(body, "token");
   }());
}

//
// An accept that nothing arrives for ends with cancel_after(), and the connection goes on
// accepting: the stream the client opens later is not lost.
//
TEST_P(QuicStreams, WHEN_accept_stream_takes_cancel_after_THEN_it_can_be_cancelled)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, none] = co_await connection.async_accept_stream(cancel_after(100ms, as_tuple));
      EXPECT_EQ(ec, errc::operation_canceled);

      auto [accept_ec, stream] =
         co_await connection.async_accept_stream(cancel_after(10s, as_tuple));
      EXPECT_FALSE(accept_ec) << accept_ec.message();
      auto [read_ec, body] = co_await read_all(stream);
      co_await stream.async_write_eof(boost::asio::buffer(body), use_awaitable);
      std::ignore = co_await connection.async_accept_stream(as_tuple(use_awaitable));
   };

   run([&]() -> Task<void> {
      auto connection = co_await quic::async_connect(context.get_executor(), "127.0.0.2", port(),
                                                     client_config(), use_awaitable);
      co_await delay(200ms); // the server's first accept has given up by now
      auto [ec, stream] = co_await connection.open_stream();
      co_await stream.async_write_eof(boost::asio::buffer("late", 4), use_awaitable);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(body, "late");
   }());
}

#endif

// =================================================================================================
