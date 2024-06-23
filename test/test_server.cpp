#include "anyhttp/client.hpp"
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process.hpp>
#include <boost/process/args.hpp>

#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include <range-v3/range/v3/view/iota.hpp>
#include <range-v3/range/v3/view/take.hpp>

using namespace std::chrono_literals;
namespace bp = boost::process;
using namespace boost::asio;
namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators;

using tcp = asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using asio::as_tuple;
using asio::awaitable;
using asio::co_spawn;
using asio::deferred;

namespace rv = ranges::views;

using namespace anyhttp;

namespace anyhttp
{
void PrintTo(Protocol protocol, std::ostream* os)
{
   switch (protocol)
   {
   case Protocol::http11:
      *os << "http11";
      break;
   case Protocol::http2:
      *os << "http2";
      break;
   default:
   }
}
}; // namespace anyhttp

// =================================================================================================

class Empty : public testing::Test
{
};

TEST_F(Empty, Hello)
{
   boost::process::filesystem::path path{"."};
   std::cout << "Hello, World!" << '\n';
   std::cout << "Path: " << path << '\n';
}

TEST_F(Empty, Path)
{
   bp::filesystem::path path("/usr/bin/echo");
   std::cout << "spawn: " << path.string() << '\n';
}

// =================================================================================================

//
// Server fixture with some default request handlers.
//
// Although the server itself supports all protocols at runtime, this is a parametrized fixture
// for use by the clients.
//
class Server : public testing::TestWithParam<anyhttp::Protocol>
{
protected:
   void SetUp() override
   {
      auto config = server::Config{.listen_address = "127.0.0.2", .port = 0};
      server.emplace(context.get_executor(), config);
      server->setRequestHandlerCoro(
         [this](server::Request request, server::Response response) -> awaitable<void>
         {
            if (request.url().path() == "/echo")
               return echo(std::move(request), std::move(response));
            else if (request.url().path() == "/eat_request")
               return eat_request(std::move(request), std::move(response));
            else if (request.url().path() == "/discard")
               return discard(std::move(request), std::move(response));
            else if (request.url().path() == "/detach")
               co_spawn(request.executor(), detach(std::move(request), std::move(response)),
                        [&](const std::exception_ptr&)
                        { logi("client finished, resetting server"); });
            else if (request.url().path() == "/custom")
               return handler(std::move(request), std::move(response));
            else
               return not_found(std::move(request), std::move(response));
            return []() mutable -> awaitable<void> { co_return; }();
         });
   }

   auto completion_handler()
   {
      return [this](const std::exception_ptr& ex)
      {
         if (ex)
            logw("client finished with {}", what(ex));
         logi("client finished, resetting server");
         server.reset();
      };
   }

protected:
   boost::asio::io_context context;
   std::optional<server::Server> server;
   std::function<awaitable<void>(server::Request request, server::Response response)> handler;
};

INSTANTIATE_TEST_SUITE_P(Server, Server,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::http2));

// -------------------------------------------------------------------------------------------------

TEST_P(Server, StopBeforeStarted)
{
   server.reset();
   context.run();
}

TEST_P(Server, Stop)
{
   context.run_one();
   server.reset();
   context.run();
}

// =================================================================================================

class External : public Server
{
protected:
   awaitable<void> log(bp::async_pipe pipe)
   {
      std::string buffer;
      for (;;)
      {
         auto [ex, n] = co_await asio::async_read_until(pipe, asio::dynamic_buffer(buffer), "\n",
                                                        as_tuple(deferred));
         auto sv = std::string_view(buffer).substr(0, n - 1);
         if (n)
            logw("STDERR: \x1b[32m{}\x1b[0m", sv);
         if (ex)
            break;
         buffer.erase(0, n);
      }
   }

   awaitable<std::string> spawn_process(bp::filesystem::path path, std::vector<std::string> args)
   {
      logi("spawn: {} {}", path.generic_string(), fmt::join(args, " "));

      bp::async_pipe out(context), err(context);
      bp::child child(
         path, std::move(args), bp::std_out > out, bp::std_err > err,
         bp::on_exit = [](int exit, const std::error_code& ec) { //
            fmt::println("exit={}, ec={}", exit, ec.message());
         });

      std::string result;
      std::vector<char> buf(4096);
      for (;;)
      {
         auto [ex, nread] = co_await asio::async_read(out, asio::buffer(buf), as_tuple(deferred));
         result += std::string_view(buf.data(), nread);
         if (ex)
         {
            logw("spwan: STDIN: {}", ex.message());
            break;
         }
      }
      co_await log(std::move(err));

      child.wait(); // FIXME: this is sync
      if (child.exit_code())
         logw("exit_code={}", child.exit_code());
      else
         logi("exit_code={}", child.exit_code());
      server.reset();
      co_return result;
   }

   auto spawn(bp::filesystem::path path, std::vector<std::string> args)
   {
      return co_spawn(context, spawn_process(std::move(path), std::move(args)), use_future);
   }

   bp::filesystem::path testFile{"CMakeLists.txt"};
   size_t testFileSize = file_size(testFile);
};

INSTANTIATE_TEST_SUITE_P(External, External,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::http2));

// -------------------------------------------------------------------------------------------------

TEST_P(External, nghttp2)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP();

   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   auto future = spawn("/workspaces/nghttp2/install/bin/nghttp", {"-d", testFile.string(), url});
   context.run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl)
{
   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   std::vector<std::string> args = {"-sS", "-v", "--data-binary",
                                    fmt::format("@{}", testFile.string()), url};
   if (GetParam() == anyhttp::Protocol::http2)
      args.insert(args.begin(), "--http2-prior-knowledge");
   auto future = spawn("/usr/bin/curl", std::move(args));
   context.run();
   loge("context finished");
   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, echo)
{
   co_spawn(context.get_executor(), spawn_process("/usr/bin/echo", {}), detached);
   context.run();
   loge("context finished");
}

// =================================================================================================

class Client : public Server
{
protected:
   void SetUp() override
   {
      Server::SetUp();
      url.set_port_number(server->local_endpoint().port());
      client::Config config{.url = url, .protocol = GetParam()};
      config.url.set_port_number(server->local_endpoint().port());
      client.emplace(context.get_executor(), config);
   }

protected:
   boost::urls::url url{"http://127.0.0.2"};
   std::optional<client::Client> client;
};

class ClientAsync : public Client
{
public:
   void SetUp() override
   {
      Client::SetUp();
      co_spawn(
         context,
         [&]() -> awaitable<void>
         {
            auto session = co_await client->async_connect(asio::deferred);
            co_await test(std::move(session));
         },
         completion_handler());
   }

public:
   std::function<awaitable<void>(Session session)> test;
};

INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::http2));

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_post_data_THEN_receive_echo)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      size_t bytes = 1024; //  * 1024 * 1024;
      auto res = co_await (send(request, bytes) && read_response(request));
      EXPECT_EQ(bytes, res);
   };
   context.run();
}

TEST_P(ClientAsync, WHEN_post_without_path_THEN_error_404)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {}, deferred);
      co_await send(request, 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
   };
   context.run();
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("unknown"), {}, deferred);
      co_await send(request, 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
   };
   context.run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_error_500)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("discard"), {}, deferred);
      co_await send(request, 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
   };
   context.run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_delayed_THEN_error_500)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("detach"), {}, deferred);
      co_await send(request, 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
   };
   context.run();
}

TEST_P(ClientAsync, Custom)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {}, deferred);
      for (;;)
      {
         auto buffer = co_await request.async_read_some(deferred);
         co_await response.async_write(asio::buffer(buffer), deferred);
         if (buffer.empty())
            co_return;
      }
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {}, deferred);
      size_t bytes = 1024;
      auto res = co_await (send(request, bytes) && read_response(request));
      EXPECT_EQ(bytes, res);
   };
   context.run();
}

TEST_P(ClientAsync, IgnoreRequest)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {}, deferred);
      co_await response.async_write({}, deferred);
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {}, deferred);
      auto res = co_await (send(request, 0) && read_response(request));
   };
   context.run();
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   { co_return; };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {}, deferred);
      auto res = co_await (send(request, 0) && read_response(request));
   };
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, PostRange)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      // co_await request.async_write(asio::buffer("ping"), deferred); // FIXME:
      auto response = co_await request.async_get_response(asio::deferred);
      // std::string s(10ul * 1024 * 1024, 'a');
      // auto sender = send(request, std::string_view("blah"));
      // auto sender = send(request, std::string(10ul * 1024 * 1024, 'a'));
      auto sender = send(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
      auto received = co_await (std::move(sender) && receive(response));
      loge("received: {}", received);
   };
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      constexpr size_t bytes = 1024;
      co_await send(request, bytes);
      EXPECT_EQ(co_await receive(response), bytes);
   };
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, EatRequest)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("eat_request"), {}, deferred);
      co_await send(request, 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
      EXPECT_EQ(received, 0);
   };
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, DISABLED_Backpressure)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      auto sender = send(request, rv::iota(uint8_t(0)), /* eof */ false);
      co_await (std::move(sender) || sleep(2s));
      auto received = co_await (sendEOF(request) && try_receive(response));
      fmt::println("transferred {} bytes", received);
   };
   context.run();
}

TEST_P(ClientAsync, Cancellation)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      // co_await sleep(100ms);
      std::vector buffer(5ul * 1024 * 1024, 'a');
      auto sender = send(request, std::string_view(buffer));
      auto received = co_await ((std::move(sender) || sleep(0ms)) && try_receive(response));
      fmt::println("received {} bytes", std::get<1>(received));
   };
   context.run();
}

TEST_P(ClientAsync, CancellationRange)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      auto sender = send(request, rv::iota(uint8_t(0)));
      auto received = co_await ((std::move(sender) || sleep(2ms)) && receive(response));
      fmt::println("received {} bytes", std::get<1>(received));
   };
   context.run();
}

// =================================================================================================
