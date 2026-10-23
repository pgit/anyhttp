#include "test_fixtures.hpp"

#include "anyhttp/quic.hpp"

#include <array>
#include <string>
#include <vector>

using namespace testing;

// =================================================================================================

namespace
{

/// Reads \p reader to its end, or to the first error: what it had, and how it ended (success
/// for a clean end).
Task<std::tuple<error_code, std::string>> read_all(Reader& reader)
{
   std::string body;
   std::array<char, 4096> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.read_some(asio::buffer(buffer));
      body.append(buffer.data(), n);
      if (ec)
         co_return std::tuple{ec == errors::eof ? error_code{} : ec, std::move(body)};
   }
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
      auto [write_ec] = co_await stream.write_eof(asio::buffer(body));
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
      auto [ec, stream] = connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_EQ(stream.id(), 0);

      auto [write_ec] = co_await stream.write(asio::buffer("Hello, ", 7));
      EXPECT_FALSE(write_ec) << write_ec.message();
      auto [eof_ec] = co_await stream.write_eof(asio::buffer("World!", 6));
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
         auto [write_ec] = co_await stream.write_eof(asio::buffer(body));
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
      co_await when_both(exchange(std::get<1>(connection.open_stream()), pattern(1024 * 1024)),
                         exchange(std::get<1>(connection.open_stream()), pattern(700 * 1024 + 17)));
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
         auto [write_ec] = co_await stream.write(asio::buffer(std::string_view(part)));
         EXPECT_FALSE(write_ec) << write_ec.message();
      }
      std::ignore = co_await stream.write_eof();
      std::ignore = co_await connection.accept_stream(); // until the client closes
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = connection.open_stream();
      auto [eof_ec] = co_await stream.write_eof(asio::buffer("question", 8));
      EXPECT_FALSE(eof_ec) << eof_ec.message();

      auto [read_ec, response] = co_await read_all(stream);
      EXPECT_FALSE(read_ec) << read_ec.message();
      EXPECT_EQ(response, "answer");

      // the end is final: data after it is refused, a second end is a no-op
      auto [late_ec] = co_await stream.write(asio::buffer("x", 1));
      EXPECT_EQ(late_ec, errc::broken_pipe);
      auto [again_ec] = co_await stream.write_eof();
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
      std::ignore = co_await stream.write_eof(asio::buffer("still here", 10));
      std::ignore = co_await connection.accept_stream();
   };

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = connection.open_stream();
      auto [write_ec] = co_await stream.write(asio::buffer("partial", 7));
      EXPECT_FALSE(write_ec) << write_ec.message();
      co_await delay(50ms); // let it arrive, so that the server sees a stream to accept
      stream.reset_stream(42);

      auto [late_ec] = co_await stream.write(asio::buffer("more", 4));
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
      auto [ec, stream] = connection.open_stream();

      //
      // Writes keep succeeding until the STOP_SENDING has arrived, a round trip after the first
      // one has. Give it a few hundred milliseconds' worth of them.
      //
      error_code write_ec;
      for (int i = 0; i < 100 && !write_ec; ++i)
      {
         std::tie(write_ec) = co_await stream.write(asio::buffer("data", 4));
         if (!write_ec)
            co_await delay(5ms);
      }
      EXPECT_EQ(write_ec, errc::connection_reset);

      auto [late_ec] = co_await stream.write(asio::buffer("more", 4));
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
      auto [ec, stream] = connection.open_stream();
      auto [write_ec] = co_await stream.write(asio::buffer("unfinished", 10));
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
      auto [ec, stream] = connection.open_stream();
      auto body = pattern(8 * window);
      auto [write_ec] = co_await stream.write_eof(asio::buffer(body));
      EXPECT_FALSE(write_ec) << write_ec.message();
      EXPECT_TRUE(reading) << "the write completed before the server started reading";
      std::ignore = co_await read_all(stream);
   }());
}

TEST_P(QuicStreamsSmallWindow, WHEN_peer_allows_no_more_streams_THEN_open_stream_would_block)
{
   serverConnection = echo_streams;
   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec1, first] = connection.open_stream();
      EXPECT_FALSE(ec1) << ec1.message();
      auto [ec2, second] = connection.open_stream();
      EXPECT_FALSE(ec2) << ec2.message();
      auto [ec3, third] = connection.open_stream();
      EXPECT_EQ(ec3, errors::would_block);
      EXPECT_FALSE(third);

      //
      // Once a stream is over, the server lets us open another one.
      //
      std::ignore = co_await first.write_eof(asio::buffer("1", 1));
      auto [read_ec, body] = co_await read_all(first);
      EXPECT_EQ(body, "1");
      first.reset();

      error_code ec4 = errors::would_block;
      quic::Stream fourth;
      for (int i = 0; i < 50 && ec4 == errors::would_block; ++i)
      {
         co_await delay(10ms);
         std::tie(ec4, fourth) = connection.open_stream();
      }
      EXPECT_FALSE(ec4) << ec4.message();
      EXPECT_EQ(fourth.id(), 8);
   }());
}

// -------------------------------------------------------------------------------------------------

TEST_P(QuicStreams, WHEN_server_opens_a_stream_THEN_client_accepts_it)
{
   serverConnection = [](quic::Connection connection) -> Task<void> {
      auto [ec, stream] = connection.open_stream();
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_EQ(stream.id(), 1); // the first server-initiated bidirectional stream
      std::ignore = co_await stream.write_eof(asio::buffer("push", 4));
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
      std::ignore = co_await stream.write_eof(asio::buffer("ack", 3));
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
      auto [ec, stream] = connection.open_stream();
      std::ignore = co_await stream.write(asio::buffer("hi", 2));

      auto [accept_ec, accepted] = co_await connection.accept_stream();
      EXPECT_EQ(accept_ec, errors::connection_aborted);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(read_ec, errors::partial_message);

      auto [open_ec, opened] = connection.open_stream();
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
      auto [ec, stream] = connection.open_stream();
      auto copy = body;
      auto result = co_await when_either(stream.write(asio::buffer(copy)), delay(100ms));
      EXPECT_EQ(result.index(), 1) << "the write completed, although nothing was read";
      copy.assign(copy.size(), '!'); // a cancelled write releases the buffer

      //
      // Staged: the stream survives, with what was copied of the body. Zero copy: so it does, if
      // what the stack was pointing into the buffer for has been acknowledged -- which, the
      // stream being blocked by flow control, it usually is. Otherwise the stream had to be
      // reset.
      //
      auto [write_ec] = co_await stream.write_eof(asio::buffer("end", 3));
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

TEST_P(QuicStreams, WHEN_http3_is_on_the_same_port_THEN_both_work)
{
   serverConnection = echo_streams;
   server->on_request([](server::Request request, server::Response response) -> Task<void> {
      co_await not_found(std::move(request), std::move(response));
   });

   run([&]() -> Task<void> {
      auto connection = co_await connect();
      auto [ec, stream] = connection.open_stream();
      std::ignore = co_await stream.write_eof(asio::buffer("plain", 5));
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
      EXPECT_EQ(message.result_int(), 404);
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
      co_await stream.async_write_eof(asio::buffer(body), use_awaitable);
      std::ignore = co_await connection.async_accept_stream(as_tuple(use_awaitable));
   };

   run([&]() -> Task<void> {
      auto connection = co_await quic::async_connect(context.get_executor(), "127.0.0.2", port(),
                                                     client_config(), use_awaitable);
      auto [ec, stream] = connection.open_stream();
      co_await stream.async_write_eof(asio::buffer("token", 5), use_awaitable);
      auto [read_ec, body] = co_await read_all(stream);
      EXPECT_EQ(body, "token");
   }());
}

#endif

// =================================================================================================
