//
// What the COROSIO port builds on, checked against the pinned capy/corosio
// (docs/corosio-port-plan.md). Should one of these break after moving a pin, it says which
// assumption went away. Empty in an ASIO build.
//
#include "anyhttp/config.hpp"

#if ANYHTTP_COROSIO

#include <boost/http/error.hpp>
#include <boost/http/request.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/response.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include "anyhttp/h1/io.hpp"
#include "anyhttp/runtime.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <vector>

using namespace testing;

namespace capy = boost::capy;
namespace http = boost::http;

// =================================================================================================

//
// Boost.HTTP's own loops around its parser and serializer -- parser::read() and serializer::sink
// -- are written for capy's streams, so they run on corosio sockets as they are. anyhttp has loops
// of its own (h1/io.hpp), to run on either runtime, but these say what Boost.HTTP takes for
// granted of a stream. A chunked request and a response with a Content-Length, so that both
// framings go through both halves.
//
TEST(Corosio, HttpParserAndSerializer)
{
   boost::corosio::io_context context;
   auto [ec, server, client] = boost::corosio::test::make_socket_pair(context);
   ASSERT_FALSE(ec);

   std::error_code server_ec, client_ec;
   http::request_parser request_parser(anyhttp::h1::make_parser_config(true, 64 * 1024));
   http::response_parser response_parser(anyhttp::h1::make_parser_config(false, 64 * 1024));
   http::serializer server_serializer(anyhttp::h1::make_serializer_config());
   http::serializer client_serializer(anyhttp::h1::make_serializer_config());
   std::string request_body, response_body;
   bool chunked = false;

   auto serve = [&](boost::corosio::tcp_socket& socket) -> capy::task<> {
      request_parser.reset();
      request_parser.start();
      if (auto [read_ec] = co_await request_parser.read(socket); read_ec)
      {
         server_ec = read_ec;
         co_return;
      }
      chunked = request_parser.get().metadata().transfer_encoding.is_chunked;
      request_body = request_parser.body();

      http::response response(http::status::ok);
      response.set_payload_size(request_body.size());
      server_serializer.set_message(response);
      auto sink = server_serializer.sink_for(socket);
      auto [write_ec, n] = co_await sink.write_eof(capy::make_buffer(request_body));
      server_ec = write_ec;
   };

   auto request = [&](boost::corosio::tcp_socket& socket) -> capy::task<> {
      http::request request(http::method::post, "/echo");
      request.set_chunked(true);
      client_serializer.set_message(request);
      auto sink = client_serializer.sink_for(socket);
      if (auto [write_ec, n] =
             co_await sink.write_eof(capy::make_buffer(std::string_view("hello, corosio")));
          write_ec)
      {
         client_ec = write_ec;
         co_return;
      }

      response_parser.reset();
      response_parser.start();
      auto [read_ec] = co_await response_parser.read(socket);
      client_ec = read_ec;
      if (!read_ec)
         response_body = response_parser.body();
   };

   capy::run_async(context.get_executor())(serve(server));
   capy::run_async(context.get_executor())(request(client));
   context.run();

   EXPECT_FALSE(server_ec) << server_ec.message();
   EXPECT_FALSE(client_ec) << client_ec.message();
   EXPECT_TRUE(chunked);
   EXPECT_EQ(request_parser.get().target(), "/echo");
   EXPECT_EQ(request_body, "hello, corosio");
   EXPECT_EQ(response_parser.get().status_int(), 200);
   EXPECT_EQ(response_body, "hello, corosio");
}

// -------------------------------------------------------------------------------------------------

//
// Why each I/O runtime needs error constants of its own: Boost.HTTP's errors are std::error_code,
// the same in both runtimes, but asio's eof does not become capy's. What docs/end-of-body.rst
// calls errors::eof at the end of a body is capy::error::eof in COROSIO.
//
TEST(Corosio, ErrorCodeInterop)
{
   const std::error_code incomplete = http::error::incomplete;
   EXPECT_EQ(incomplete, std::error_code(boost::system::error_code(incomplete)));

   const std::error_code eof = boost::system::error_code{boost::asio::error::eof};
   EXPECT_NE(eof, capy::cond::eof);
   EXPECT_EQ(std::error_code{capy::error::eof}, capy::cond::eof);

   //
   // What the parser itself reports, and what anyhttp makes of it.
   //
   http::request_parser parser(anyhttp::h1::make_parser_config(true, 32));
   parser.reset();
   parser.start();
   std::string_view request = "GET / HTTP/1.1\r\nX-Field: a value well over sixteen bytes\r\n";
   auto buffers = parser.prepare();
   const auto n = capy::buffer_copy(buffers, capy::make_buffer(request));
   parser.commit(n);
   std::error_code limit;
   parser.parse(limit);
   EXPECT_EQ(anyhttp::h1::to_error_code(limit), anyhttp::errors::header_limit) << limit.message();
}

// =================================================================================================
// The COROSIO half of the runtime layer (corosio/runtime.hpp), without any protocol on top.
// =================================================================================================

namespace
{

using namespace anyhttp;
using ReadSome = void(error_code, size_t);

//
// A stand-in for a backend that parks operations, as HTTP/2 and HTTP/3 do: the parked completion
// is completed by whoever calls finish().
//
struct Parked
{
   Completion<ReadSome> handler;
   int cancelled = 0;

   auto read()
   {
      return initiate<ReadSome>([this](Completion<ReadSome> h) {
         handler = std::move(h);
         on_cancel(handler, [this] {
            ++cancelled;
            if (handler)
               complete(std::move(handler), Executor{}, errors::canceled, size_t{0});
         });
      });
   }

   void finish(size_t n) { complete(std::move(handler), Executor{}, error_code{}, n); }
};

Task<void> finish_later(Parked& parked, size_t n)
{
   co_await yield_now();
   parked.finish(n);
}

} // namespace

TEST(CorosioRuntime, WHEN_parked_operation_is_completed_THEN_caller_resumes_with_result)
{
   boost::corosio::io_context context;
   Parked parked;
   std::optional<results<size_t>> result;

   auto reader = [](Parked& parked, auto& result) -> Task<void> {
      result = co_await parked.read();
   };
   launch(context.get_executor(), reader(parked, result));
   launch(context.get_executor(), finish_later(parked, 42));
   context.run();

   ASSERT_TRUE(result);
   EXPECT_FALSE(std::get<0>(*result));
   EXPECT_EQ(std::get<1>(*result), 42);
}

TEST(CorosioRuntime, WHEN_caller_is_stopped_THEN_parked_operation_is_cancelled)
{
   boost::corosio::io_context context;
   Parked parked;
   std::stop_source stop;
   std::optional<results<size_t>> result;

   auto reader = [](Parked& parked, auto& result) -> Task<void> {
      result = co_await parked.read();
   };
   capy::run_async(context.get_executor(), stop.get_token())(reader(parked, result));
   run_later(context.get_executor(), [&] { stop.request_stop(); }); // once the read is parked
   context.run();

   ASSERT_TRUE(result);
   EXPECT_EQ(std::get<0>(*result), capy::cond::canceled);
   EXPECT_EQ(parked.cancelled, 1);
}

TEST(CorosioRuntime, WHEN_completion_is_dropped_THEN_caller_resumes_cancelled)
{
   boost::corosio::io_context context;
   Parked parked;
   std::optional<results<size_t>> result;

   auto reader = [](Parked& parked, auto& result) -> Task<void> {
      result = co_await parked.read();
   };
   launch(context.get_executor(), reader(parked, result));
   run_later(context.get_executor(), [&] { parked.handler = nullptr; }); // once it is parked
   context.run();

   ASSERT_TRUE(result);
   EXPECT_EQ(std::get<0>(*result), capy::cond::canceled);
}

TEST(CorosioRuntime, WHEN_task_is_launched_for_a_completion_THEN_its_result_completes_it)
{
   boost::corosio::io_context context;
   std::optional<results<size_t>> result;

   auto task = []() -> IoTask<size_t> {
      co_await yield_now();
      co_return std::tuple{errors::eof, size_t{7}};
   };
   auto caller = [](auto task, auto& result) -> Task<void> {
      auto executor = co_await capy::this_coro::executor;
      result = co_await initiate<ReadSome>([&](Completion<ReadSome> handler) {
         launch(Executor(executor), task(), std::move(handler));
      });
   };
   launch(context.get_executor(), caller(task, result));
   context.run();

   ASSERT_TRUE(result);
   EXPECT_EQ(std::get<0>(*result), capy::cond::eof);
   EXPECT_EQ(std::get<1>(*result), 7);
}

//
// Destroying a context unwinds the coroutines suspended on it, but only those whose continuation
// carries the owner of their chain. A frame that is awaited is what shows the difference: without
// an owner, only the frame on top is destroyed, and the one below it stays behind, holding a
// destroyed frame. These go by what the frames do, not by LeakSanitizer: capy pools frames.
//
namespace
{
struct FrameProbe
{
   int& destroyed;
   ~FrameProbe() { ++destroyed; }
};
} // namespace

TEST(CorosioRuntime,
     WHEN_context_goes_away_with_a_nested_yield_now_queued_THEN_frames_are_destroyed)
{
   int destroyed = 0, resumed = 0;
   {
      boost::corosio::io_context context;
      auto inner = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         co_await yield_now();
         ++resumed;
      };
      auto outer = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         co_await inner();
         ++resumed;
      };
      launch(context.get_executor(), outer());
      context.run_one();
   }
   EXPECT_EQ(destroyed, 2);
   EXPECT_EQ(resumed, 0);
}

TEST(CorosioRuntime, WHEN_context_goes_away_after_a_nested_completion_was_THEN_frames_are_destroyed)
{
   int destroyed = 0, resumed = 0;
   {
      boost::corosio::io_context context;
      Completion<ReadSome> parked;
      auto inner = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         co_await initiate<ReadSome>(
            [&](Completion<ReadSome> handler) { parked = std::move(handler); });
         ++resumed;
      };
      auto outer = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         co_await inner();
         ++resumed;
      };
      launch(context.get_executor(), outer());
      context.run_one();
      ASSERT_TRUE(parked);
      parked = nullptr; // completes as cancelled, which is posted and never runs
   }
   EXPECT_EQ(destroyed, 2);
   EXPECT_EQ(resumed, 0);
}

TEST(CorosioRuntime, WHEN_context_goes_away_with_a_task_launched_for_a_completion_THEN_all_unwinds)
{
   int destroyed = 0, resumed = 0;
   {
      boost::corosio::io_context context;
      auto task = [&]() -> IoTask<size_t> {
         FrameProbe probe{destroyed};
         co_await yield_now();
         ++resumed;
         co_return std::tuple{error_code{}, size_t{0}};
      };
      auto inner = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         auto executor = co_await capy::this_coro::executor;
         co_await initiate<ReadSome>([&](Completion<ReadSome> handler) {
            launch(Executor(executor), task(), std::move(handler));
         });
         ++resumed;
      };
      auto outer = [&]() -> Task<void> {
         FrameProbe probe{destroyed};
         co_await inner();
         ++resumed;
      };
      launch(context.get_executor(), outer());
      context.run_one();
   }
   EXPECT_EQ(destroyed, 3);
   EXPECT_EQ(resumed, 0);
}

TEST(CorosioRuntime, WHEN_event_is_set_before_and_while_waiting_THEN_waits_end)
{
   boost::corosio::io_context context;
   Event event;
   int woken = 0;

   auto waiter = [](Event& event, int& woken) -> Task<void> {
      auto [ec] = co_await event.wait(); // already set: does not wait
      EXPECT_FALSE(ec);
      ++woken;
      event.clear();
      auto [ec2] = co_await event.wait();
      EXPECT_FALSE(ec2);
      ++woken;
   };
   event.set();
   launch(context.get_executor(), waiter(event, woken));
   run_later(context.get_executor(), [&] { event.set(); });
   context.run();
   EXPECT_EQ(woken, 2);
}

TEST(CorosioRuntime, WHEN_both_tasks_run_THEN_when_both_waits_for_both)
{
   boost::corosio::io_context context;
   std::vector<int> done;

   auto one = [](std::vector<int>& done, int id) -> Task<void> {
      for (int i = 0; i < id; ++i)
         co_await yield_now();
      done.push_back(id);
   };
   auto both = [](auto one, std::vector<int>& done) -> Task<void> {
      co_await when_both(one(done, 2), one(done, 1));
      done.push_back(0);
   };
   launch(context.get_executor(), both(one, done));
   context.run();
   EXPECT_THAT(done, ElementsAre(1, 2, 0));
}

TEST(CorosioRuntime, WHEN_peeking_THEN_bytes_are_seen_but_not_taken)
{
   boost::corosio::io_context context;
   auto [ec, server, client] = boost::corosio::test::make_socket_pair(context);
   ASSERT_FALSE(ec);
   std::string peeked, read;

   auto run = [](boost::corosio::tcp_socket& server, boost::corosio::tcp_socket& client,
                 std::string& peeked, std::string& read) -> Task<void> {
      std::ignore = co_await capy::write(client, capy::const_buffer("\x16hello", 6));
      std::array<char, 1> first;
      auto [ec, n] = co_await io::peek(server, boost::asio::buffer(first));
      EXPECT_FALSE(ec);
      peeked.assign(first.data(), n);
      std::array<char, 16> all;
      auto [read_ec, m] = co_await io::read_some(server, boost::asio::buffer(all));
      EXPECT_FALSE(read_ec);
      read.assign(all.data(), m);
   };
   launch(context.get_executor(), run(server, client, peeked, read));
   context.run();
   EXPECT_EQ(peeked, "\x16");
   EXPECT_EQ(read, "\x16hello");
}

// =================================================================================================

#endif // ANYHTTP_COROSIO
