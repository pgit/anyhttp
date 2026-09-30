//
// What the CAPY port builds on, checked against the pinned capy/corosio (docs/capy-port-plan.md).
// Should one of these break after moving a pin, it says which assumption went away. Empty in an
// ASIO build.
//
#if ANYHTTP_CAPY

#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include "anyhttp/runtime.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <vector>

using namespace testing;

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::beast::http;

// =================================================================================================

namespace
{

//
// Reads from `socket` into `parser` until it has a complete message. put() consumes only part of
// what it is given -- the header first, and the body in a call of its own -- so everything it
// leaves has to be offered again.
//
template <class Parser>
capy::task<std::error_code> read_message(corosio::tcp_socket& socket, Parser& parser)
{
   std::array<char, 4096> buffer;
   while (!parser.is_done())
   {
      auto [ec, n] = co_await socket.read_some(capy::mutable_buffer(buffer.data(), buffer.size()));
      if (ec)
         co_return ec;

      for (const char* data = buffer.data(); n > 0 && !parser.is_done();)
      {
         boost::system::error_code bec;
         const auto used = parser.put(boost::asio::buffer(data, n), bec);
         if (bec == http::error::need_more)
            break;
         if (bec)
            co_return bec;
         data += used, n -= used;
      }
   }
   co_return std::error_code{};
}

//
// Writes what `serializer` produces to `socket`. The buffers next() hands out are valid until
// consume(), so their descriptors are copied out of the visitor and consumed after the write.
//
template <class Serializer>
capy::task<std::error_code> write_message(corosio::tcp_socket& socket, Serializer& serializer)
{
   while (!serializer.is_done())
   {
      boost::system::error_code bec;
      std::vector<capy::const_buffer> buffers;
      serializer.next(bec, [&](boost::system::error_code&, const auto& sequence) {
         for (auto buffer : boost::beast::buffers_range_ref(sequence))
            buffers.emplace_back(buffer.data(), buffer.size());
      });
      if (bec)
         co_return bec;

      auto [ec, n] = co_await capy::write(socket, buffers);
      if (ec)
         co_return ec;
      serializer.consume(n);
   }
   co_return std::error_code{};
}

} // namespace

// =================================================================================================

//
// HTTP/1.1 without Beast's asynchronous operations, which need an ASIO stream: its parser and
// serializer, driven by read and write loops on corosio sockets. A chunked request and a response
// with a Content-Length, so that both framings go through both halves.
//
TEST(Corosio, BeastParserAndSerializer)
{
   corosio::io_context context;
   auto [server, client] = corosio::test::make_socket_pair(context);

   std::error_code server_ec, client_ec;
   http::request_parser<http::string_body> request_parser;
   http::response_parser<http::string_body> response_parser;

   auto serve = [](corosio::tcp_socket& socket, auto& parser,
                   std::error_code& result) -> capy::task<> {
      if ((result = co_await read_message(socket, parser)))
         co_return;

      http::response<http::string_body> response{http::status::ok, 11, parser.get().body()};
      response.prepare_payload();
      http::response_serializer<http::string_body> serializer{response};
      result = co_await write_message(socket, serializer);
   };

   auto request = [](corosio::tcp_socket& socket, auto& parser,
                     std::error_code& result) -> capy::task<> {
      http::request<http::string_body> request{http::verb::post, "/echo", 11, "hello, corosio"};
      request.chunked(true);
      http::request_serializer<http::string_body> serializer{request};
      if ((result = co_await write_message(socket, serializer)))
         co_return;

      result = co_await read_message(socket, parser);
   };

   capy::run_async(context.get_executor())(serve(server, request_parser, server_ec));
   capy::run_async(context.get_executor())(request(client, response_parser, client_ec));
   context.run();

   EXPECT_FALSE(server_ec) << server_ec.message();
   EXPECT_FALSE(client_ec) << client_ec.message();
   EXPECT_TRUE(request_parser.chunked());
   EXPECT_EQ(request_parser.get().target(), "/echo");
   EXPECT_EQ(response_parser.get().result_int(), 200);
   EXPECT_EQ(response_parser.get().body(), "hello, corosio");
}

// -------------------------------------------------------------------------------------------------

//
// Why each API style needs error constants of its own: a Boost.System code survives the conversion
// to std::error_code -- Beast's errors stay comparable across it -- but asio's eof does not become
// capy's. The README's "asio::error::eof at the end of a body" is capy::error::eof in CAPY.
//
TEST(Corosio, ErrorCodeInterop)
{
   const std::error_code partial = boost::system::error_code{http::error::partial_message};
   EXPECT_EQ(partial, std::error_code(boost::system::error_code{http::error::partial_message}));
   EXPECT_EQ(partial.message(), "partial message");

   const std::error_code eof = boost::system::error_code{boost::asio::error::eof};
   EXPECT_NE(eof, capy::cond::eof);
   EXPECT_EQ(std::error_code{capy::error::eof}, capy::cond::eof);
}

// =================================================================================================
// The CAPY half of the runtime layer (detail/runtime_capy.hpp), without any protocol on top.
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
               complete_later(std::move(handler), Executor{}, errors::canceled, size_t{0});
         });
      });
   }

   void finish(size_t n) { complete_later(std::move(handler), Executor{}, error_code{}, n); }
};

Task<void> finish_later(Parked& parked, size_t n)
{
   co_await yield_now();
   parked.finish(n);
}

} // namespace

TEST(CapyRuntime, WHEN_parked_operation_is_completed_THEN_caller_resumes_with_result)
{
   corosio::io_context context;
   Parked parked;
   std::optional<std::tuple<error_code, size_t>> result;

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

TEST(CapyRuntime, WHEN_caller_is_stopped_THEN_parked_operation_is_cancelled)
{
   corosio::io_context context;
   Parked parked;
   std::stop_source stop;
   std::optional<std::tuple<error_code, size_t>> result;

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

TEST(CapyRuntime, WHEN_completion_is_dropped_THEN_caller_resumes_cancelled)
{
   corosio::io_context context;
   Parked parked;
   std::optional<std::tuple<error_code, size_t>> result;

   auto reader = [](Parked& parked, auto& result) -> Task<void> {
      result = co_await parked.read();
   };
   launch(context.get_executor(), reader(parked, result));
   run_later(context.get_executor(), [&] { parked.handler = nullptr; }); // once it is parked
   context.run();

   ASSERT_TRUE(result);
   EXPECT_EQ(std::get<0>(*result), capy::cond::canceled);
}

TEST(CapyRuntime, WHEN_task_is_launched_for_a_completion_THEN_its_result_completes_it)
{
   corosio::io_context context;
   std::optional<std::tuple<error_code, size_t>> result;

   auto task = []() -> Task<std::tuple<error_code, size_t>> {
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

TEST(CapyRuntime, WHEN_event_is_set_before_and_while_waiting_THEN_waits_end)
{
   corosio::io_context context;
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

TEST(CapyRuntime, WHEN_timer_is_rearmed_or_cancelled_THEN_only_the_last_callback_runs)
{
   corosio::io_context context;
   Timer timer(context.get_executor());
   std::vector<int> fired;

   timer.arm(std::chrono::milliseconds(1), [&] { fired.push_back(1); });
   timer.arm(std::chrono::milliseconds(2), [&] { fired.push_back(2); });
   Timer other(context.get_executor());
   other.arm(std::chrono::milliseconds(1), [&] { fired.push_back(3); });
   other.cancel();
   context.run();
   EXPECT_THAT(fired, ElementsAre(2));
}

TEST(CapyRuntime, WHEN_both_tasks_run_THEN_when_both_waits_for_both)
{
   corosio::io_context context;
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

TEST(CapyRuntime, WHEN_peeking_THEN_bytes_are_seen_but_not_taken)
{
   corosio::io_context context;
   auto [server, client] = corosio::test::make_socket_pair(context);
   std::string peeked, read;

   auto run = [](corosio::tcp_socket& server, corosio::tcp_socket& client, std::string& peeked,
                 std::string& read) -> Task<void> {
      std::ignore = co_await capy::write(client, capy::const_buffer("\x16hello", 6));
      std::array<char, 1> first;
      auto [ec, n] = co_await io::peek(server, asio::buffer(first));
      EXPECT_FALSE(ec);
      peeked.assign(first.data(), n);
      std::array<char, 16> all;
      auto [read_ec, m] = co_await io::read_some(server, asio::buffer(all));
      EXPECT_FALSE(read_ec);
      read.assign(all.data(), m);
   };
   launch(context.get_executor(), run(server, client, peeked, read));
   context.run();
   EXPECT_EQ(peeked, "\x16");
   EXPECT_EQ(read, "\x16hello");
}

// =================================================================================================

#endif // ANYHTTP_CAPY
