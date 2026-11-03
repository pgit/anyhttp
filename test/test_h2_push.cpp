#include "test_fixtures.hpp"

#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"

#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace testing;

// =================================================================================================
// HTTP/2 server push. The client has no way to hand out a pushed response, and asks not to get any
// with SETTINGS_ENABLE_PUSH=0. nghttp2 acts on that without involving us: a push is cancelled until
// the server has acknowledged the setting, and a connection error after that.
//
// nghttp2 version 2 knows nothing about push: it has no such setting, and any push is a connection
// error.
//
// The peer writes its frames by hand. A peer on top of nghttp2 reads the client's SETTINGS along
// with the request, and from then on, nghttp2 refuses to submit a push.
// =================================================================================================

namespace
{

constexpr uint8_t frame_data = 0x0;
constexpr uint8_t frame_headers = 0x1;
constexpr uint8_t frame_rst_stream = 0x3;
constexpr uint8_t frame_settings = 0x4;
constexpr uint8_t frame_push_promise = 0x5;
constexpr uint8_t frame_goaway = 0x7;

constexpr uint8_t flag_end_stream = 0x1;
constexpr uint8_t flag_ack = 0x1;
constexpr uint8_t flag_end_headers = 0x4;

constexpr uint16_t settings_enable_push = 0x2;
constexpr uint32_t error_protocol = 0x1;
constexpr uint32_t error_cancel = 0x8;

constexpr std::string_view client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

struct Frame
{
   uint8_t type = 0;
   uint8_t flags = 0;
   uint32_t stream_id = 0;
   std::string payload;

   /// The 32-bit value at \p offset of the payload, in network byte order.
   uint32_t u32(size_t offset = 0) const
   {
      uint32_t value = 0;
      for (size_t i = 0; i < 4; ++i)
         value = (value << 8) | static_cast<uint8_t>(payload.at(offset + i));
      return value;
   }
};

std::string u32(uint32_t value)
{
   return {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
           static_cast<char>(value >> 8), static_cast<char>(value)};
}

std::string encode(uint8_t type, uint8_t flags, uint32_t stream_id, std::string_view payload)
{
   std::string frame = u32(static_cast<uint32_t>(payload.size())).substr(1);
   frame += static_cast<char>(type);
   frame += static_cast<char>(flags);
   frame += u32(stream_id);
   frame += payload;
   return frame;
}

/// A header block in the simplest way HPACK has: literals, not indexed, each shorter than 127.
std::string hpack(std::initializer_list<std::pair<std::string_view, std::string_view>> fields)
{
   std::string block;
   for (const auto& [name, value] : fields)
   {
      block += '\0';
      block += static_cast<char>(name.size());
      block += name;
      block += static_cast<char>(value.size());
      block += value;
   }
   return block;
}

std::string push_promise(uint32_t stream_id, uint32_t promised_stream_id)
{
   auto fields = hpack({{":method", "GET"},
                        {":scheme", "http"},
                        {":authority", "example.com"},
                        {":path", "/pushed"},
                        {"x-promised", "yes"}});
   return encode(frame_push_promise, flag_end_headers, stream_id, u32(promised_stream_id) + fields);
}

std::string response(uint32_t stream_id, std::string_view body)
{
   return encode(frame_headers, flag_end_headers, stream_id, hpack({{":status", "200"}})) +
          encode(frame_data, flag_end_stream, stream_id, body);
}

/// The other end of the connection, frame by frame. Keeps every frame received, for the test.
class Peer
{
public:
   explicit Peer(TcpSocket socket) : socket_(std::move(socket)) {}

   Task<bool> write(std::string frames)
   {
      auto [ec, written] = co_await io::write(socket_, asio::buffer(frames));
      co_return !ec;
   }

   /// Next frame, or nothing if the connection has ended.
   Task<std::optional<Frame>> read_frame()
   {
      std::array<char, 16_k> chunk;
      for (;;)
      {
         if (buffer_.starts_with(client_preface))
            buffer_.erase(0, client_preface.size());

         if (buffer_.size() >= 9)
         {
            const auto byte = [this](size_t i) { return static_cast<uint8_t>(buffer_[i]); };
            const size_t length = (size_t{byte(0)} << 16) | (size_t{byte(1)} << 8) | byte(2);
            if (buffer_.size() >= 9 + length)
            {
               Frame frame{.type = byte(3),
                           .flags = byte(4),
                           .stream_id = (uint32_t{byte(5)} << 24) | (uint32_t{byte(6)} << 16) |
                                        (uint32_t{byte(7)} << 8) | byte(8),
                           .payload = buffer_.substr(9, length)};
               buffer_.erase(0, 9 + length);
               received.push_back(frame);
               co_return frame;
            }
         }

         auto [ec, n] = co_await io::read_some(socket_, asio::buffer(chunk));
         if (ec)
            co_return std::nullopt;
         buffer_.append(chunk.data(), n);
      }
   }

   /// Reads up to the end of the request on the next stream, and returns the ID of that stream.
   Task<uint32_t> read_request()
   {
      for (;;)
      {
         auto frame = co_await read_frame();
         if (!frame)
            co_return 0;
         if (frame->stream_id % 2 == 1 && (frame->flags & flag_end_stream) &&
             (frame->type == frame_headers || frame->type == frame_data))
            co_return frame->stream_id;
      }
   }

   std::vector<Frame> received;

private:
   TcpSocket socket_;
   std::string buffer_;
};

} // namespace

class H2Push : public Server
{
protected:
   void TearDown() override
   {
      Server::TearDown();
      // only now that every thread has stopped: the logger's sinks are a plain vector
      if (log_sink)
         std::erase(spdlog::default_logger_raw()->sinks(), log_sink);
   }

   /// Captures the log from now on, at info level. setup_logging() in the next SetUp() restores it.
   std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> capture_log()
   {
      auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256);
      log_sink = sink;
      spdlog::default_logger_raw()->sinks().push_back(sink);
      spdlog::set_level(spdlog::level::info);
      return sink;
   }

   using Script = std::function<Task<void>(Peer&)>;
   using ClientMain = std::function<Task<void>(Session, boost::urls::url)>;

   /**
    * Connects a client to a peer that plays \p script. Whatever the script leaves unread is read
    * up to the end of the connection, so that the test can look at all of it in \ref received.
    */
   void run_against(Script script, ClientMain client_main)
   {
      auto acceptor = io::make_acceptor(context.get_executor());
      io::listen(acceptor, {io::make_address("127.0.0.2"), 0});

      boost::urls::url target{"http://127.0.0.2/"};
      target.set_port_number(io::local_endpoint(acceptor).port());

      auto peer_main = [&]() -> Task<void> {
         auto socket = io::make_socket(context.get_executor());
         check(co_await io::accept(acceptor, socket));
         Peer peer(std::move(socket));
         co_await peer.write(encode(frame_settings, 0, 0, ""));
         co_await script(peer);
         for (;;)
         {
            auto frame = co_await peer.read_frame();
            if (!frame)
               break;
         }
         received = std::move(peer.received);
      };
      launch(context.get_executor(), peer_main(),
             [](const std::exception_ptr& ex) { EXPECT_FALSE(ex) << what(ex); });

      // on several threads, the client needs a strand, as the fixture's client has
      auto executor = threads() > 1 ? new_strand(context.get_executor()) : context.get_executor();
      client::Client client(executor, {.url = target, .protocol = Protocol::h2});

      auto client_task = [&]() -> Task<void> {
         auto session = check(co_await client.connect());
         co_await client_main(std::move(session), target);
      };
      launch(client.get_executor(), client_task(), [&](const std::exception_ptr& ex) {
         EXPECT_FALSE(ex) << what(ex);
         io::close(acceptor);
         server.reset();
      });

      run();
   }

   /// All frames the client has sent of the given type on the given stream.
   std::vector<Frame> sent(uint8_t type, uint32_t stream_id) const
   {
      std::vector<Frame> frames;
      std::ranges::copy_if(received, std::back_inserter(frames), [&](const Frame& frame) {
         return frame.type == type && frame.stream_id == stream_id;
      });
      return frames;
   }

   std::shared_ptr<spdlog::sinks::sink> log_sink;
   std::vector<Frame> received;
};

INSTANTIATE_TEST_SUITE_P(H2Push, H2Push, Values(anyhttp::Protocol::h2), NameGenerator);

TEST_P(H2Push, WHEN_connecting_THEN_the_client_asks_for_no_push)
{
   if (ANYHTTP_H2_V2)
      GTEST_SKIP() << "version 2 has no such setting, for it any push is a connection error";

   run_against(
      [](Peer& peer) -> Task<void> {
         auto stream_id = co_await peer.read_request();
         co_await peer.write(response(stream_id, "done"));
      },
      [](Session session, boost::urls::url target) -> Task<void> {
         check(co_await session.get(target));
      });

   //
   // The first thing the client says after its preface, as RFC 9113 wants, and with the push
   // setting among the ones it names.
   //
   ASSERT_FALSE(received.empty());
   const auto& settings = received.front();
   ASSERT_EQ(settings.type, frame_settings);
   ASSERT_EQ(settings.flags & flag_ack, 0);

   std::optional<uint32_t> enable_push;
   for (size_t i = 0; i + 6 <= settings.payload.size(); i += 6)
   {
      const auto high = static_cast<uint8_t>(settings.payload[i]);
      const auto low = static_cast<uint8_t>(settings.payload[i + 1]);
      if (((high << 8) | low) == settings_enable_push)
         enable_push = settings.u32(i + 2);
   }
   EXPECT_EQ(enable_push, 0u);
}

TEST_P(H2Push, WHEN_a_push_beats_the_settings_ack_THEN_it_is_logged_and_cancelled)
{
   if (ANYHTTP_H2_V2)
      GTEST_SKIP() << "version 2 has no such setting, for it any push is a connection error";

   auto sink = capture_log();

   run_against(
      [](Peer& peer) -> Task<void> {
         //
         // Not having acknowledged the client's SETTINGS, the peer is still allowed to push. The
         // response to the push comes along right away, before the reset can reach the peer.
         //
         auto stream_id = co_await peer.read_request();
         EXPECT_EQ(stream_id, 1u);
         co_await peer.write(push_promise(stream_id, 2) +
                             encode(frame_headers, flag_end_headers, 2,
                                    hpack({{":status", "200"}, {"x-pushed", "yes"}})) +
                             encode(frame_data, flag_end_stream, 2, "pushed") +
                             response(stream_id, "first"));
         co_await peer.write(encode(frame_settings, flag_ack, 0, ""));

         stream_id = co_await peer.read_request();
         EXPECT_EQ(stream_id, 3u);
         co_await peer.write(response(stream_id, "second"));
      },
      [](Session session, boost::urls::url target) -> Task<void> {
         auto first = check(co_await session.get(target));
         EXPECT_EQ(first.result_int(), 200);
         EXPECT_EQ(first.body(), "first");
         EXPECT_EQ(first["x-promised"], "") << "the promise is not part of this response";
         EXPECT_EQ(first["x-pushed"], "") << "the pushed response is not part of this one";

         auto second = check(co_await session.get(target));
         EXPECT_EQ(second.body(), "second") << "the session has to stay usable";
      });

   EXPECT_THAT(sink->last_formatted(), Contains(HasSubstr("PUSH_PROMISE, length=")));

   const auto resets = sent(frame_rst_stream, 2);
   ASSERT_FALSE(resets.empty());
   EXPECT_EQ(resets.front().u32(), error_cancel);
}

TEST_P(H2Push, WHEN_a_push_follows_the_settings_ack_THEN_it_is_a_connection_error)
{
   run_against(
      [](Peer& peer) -> Task<void> {
         auto stream_id = co_await peer.read_request();
         co_await peer.write(encode(frame_settings, flag_ack, 0, "") + push_promise(stream_id, 2));
      },
      [](Session session, boost::urls::url target) -> Task<void> {
         auto [ec, message] = co_await session.get(target);
         EXPECT_TRUE(ec) << "the connection is gone, and the request with it";
      });

   const auto goaways = sent(frame_goaway, 0);
   ASSERT_FALSE(goaways.empty());
   EXPECT_EQ(goaways.front().u32(4), error_protocol);
}
