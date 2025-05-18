#include "anyhttp/client.hpp"
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>

#include <boost/filesystem/path.hpp>

#include <boost/process.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/async.hpp>
#include <boost/process/v1/async_pipe.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/io.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/url.hpp>

#include <future>
#include <nghttp2/nghttp2ver.h>
#include <regex>

#include <gtest/gtest.h>

#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include <range/v3/view/iota.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/transform.hpp>

using namespace std::chrono_literals;
namespace bp = boost::process::v1;
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

std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
};

auto error_future(std::future<std::exception_ptr>& future)
{
   std::promise<std::exception_ptr> promise;
   future = promise.get_future();
   return [promise = std::move(promise)](std::exception_ptr ptr) mutable
   {
      promise.set_value(std::move(ptr)); //
   };
}

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

class ClientConnect : public testing::Test
{
};

TEST_F(ClientConnect, DISABLED_ErrorResolve)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://this-domain-does-not-exist:12345")};
   client::Client client(context.get_executor(), config);
   // TODO: test per-operation cancellation
   // https://live.boost.org/doc/libs/1_86_0/doc/html/boost_asio/reference/co_composed.html
   client.async_connect(
      [&](boost::system::error_code ec, Session session)
      {
         loge("ERROR: {}", ec.message());
         EXPECT_TRUE(ec);
      });
   context.run();
}

TEST_F(ClientConnect, ErrorNetworkUnreachable)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://255.255.255.255:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect(
      [&](boost::system::error_code ec, Session session)
      {
         loge("ERROR: {}", ec.message());
         EXPECT_TRUE(ec);
      });
   context.run();
}

// =================================================================================================

// #define MULTITHREADED

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
#if !defined(NDEBUG)
      spdlog::set_level(spdlog::level::debug);
#else
      spdlog::set_level(spdlog::level::info);
#endif

      auto config = server::Config{.listen_address = "127.0.0.2", .port = 0};
#if defined(MULTITHREADED)
      config.use_strand = true;
#endif
      //
      // The main server acceptor loop does not need to run on a strand. Instead, a per-connection
      // strand is created after accepting a new connection.
      //
      server.emplace(context.get_executor(), config);
      server->setRequestHandlerCoro(
         [this](server::Request request, server::Response response) -> awaitable<void>
         {
            logd("{}", request.url().path());
            if (request.url().path() == "/echo")
               return echo(std::move(request), std::move(response));
            else if (request.url().path() == "/eat_request")
               return eat_request(std::move(request), std::move(response));
            else if (request.url().path() == "/discard")
               return discard(std::move(request), std::move(response));
            else if (request.url().path() == "/detach")
            {
               co_spawn(server->executor(), detach(std::move(request), std::move(response)),
                        [&](const std::exception_ptr&) { logi("client finished"); });
               return []() mutable -> awaitable<void> { co_return; }();
            }
            else if (request.url().path() == "/custom")
               return handler(std::move(request), std::move(response));
            else
               // return not_found(std::move(request), std::move(response));
               return not_found(std::move(response)); // discard request
         });
   }

   void run()
   {
#if defined(MULTITHREADED)
      auto threads = rv::iota(0) | rv::take(std::max(1u, std::thread::hardware_concurrency())) |
                     rv::transform([&](int) { return std::thread([&] { ::run(context); }); }) |
                     ranges::to<std::vector>();

      ::run(context);

      for (auto& thread : threads)
         thread.join();
#else
      ::run(context);
#endif
   }

protected:
   boost::asio::io_context context;
   std::optional<server::Server> server;
   std::function<awaitable<void>(server::Request request, server::Response response)> handler;
};

INSTANTIATE_TEST_SUITE_P(Server, Server,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(Server, StopBeforeStarted)
{
   server.reset();
   run();
}

TEST_P(Server, Stop)
{
   context.run_one();
   server.reset();
   run();
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
         auto [ex, n] = co_await asio::async_read_until(pipe, asio::dynamic_buffer(buffer), '\n',
                                                        as_tuple(deferred));

         if (n)
         {
            auto sv = std::string_view(buffer).substr(0, n - 1);
            logw("STDERR: \x1b[32m{}\x1b[0m", sv);
            buffer.erase(0, n);
         }

         if (ex)
            break;
      }

      if (!buffer.empty())
         logw("STDERR: \x1b[32m{}\x1b[0m", buffer);
   }

   awaitable<std::string> consume(bp::async_pipe pipe)
   {
      std::string result;
      std::vector<char> buf(1460);
      for (;;)
      {
         auto [ex, nread] = co_await asio::async_read(pipe, asio::buffer(buf), as_tuple(deferred));
         result += std::string_view(buf.data(), nread);
         if (nread)
            logi("STDOUT: {} bytes", nread);
         if (ex)
            break;
      }
      co_return result;
   }

   awaitable<std::string> spawn_process(bp::filesystem::path path, std::vector<std::string> args)
   {
      logi("spawn: {} {}", path.generic_string(), fmt::join(args, " "));

      auto env = bp::environment();
      env["LD_LIBRARY_PATH"] = "/usr/local/lib";
      // env["LD_LIBRARY_PATH"] = "/workspaces/nghttp2/install/lib";
      // env["LD_LIBRARY_PATH"] = "/workspaces/nghttp2/install/lib:/usr/local/lib";
      bp::async_pipe out(context), err(context);
      bp::child child(
         path, env, std::move(args), bp::std_out > out, bp::std_err > err,
         bp::on_exit = [](int exit, const std::error_code& ec) { //
            fmt::println("exit={}, ec={}", exit, ec.message());
         });

      logi("spawn: starting to communicate...");
#if 1
      auto result = co_await (log(std::move(err)) && consume(std::move(out)));
#else
      co_await (log(std::move(err)) && log(std::move(out)));
      auto result = std::string();
#endif
      logi("spawn: starting to communicate... done");

      child.wait(); // FIXME: this is sync
      if (child.exit_code())
         logw("exit_code={}", child.exit_code());
      else
         logi("exit_code={}", child.exit_code());

      if (--numSpawned <= 0)
         server.reset();

      co_return result;
   }

   std::future<std::string> spawn(bp::filesystem::path path, std::vector<std::string> args)
   {
      ++numSpawned;
      std::future<std::string> future;
      std::promise<std::string> promise;
      future = promise.get_future();
      co_spawn(context, spawn_process(std::move(path), std::move(args)),
               [promise = std::move(promise)](const std::exception_ptr& ex, std::string str) mutable
               {
                  if (ex)
                  {
                     str = what(ex);
                     loge("{}", str);
                  }
                  promise.set_value(std::move(str));
               });
      return std::move(future);
   }

   bp::filesystem::path testFile{"CMakeLists.txt"};
   size_t testFileSize = file_size(testFile);
   int numSpawned = 0;
};

INSTANTIATE_TEST_SUITE_P(External, External,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(External, nghttp2)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // no --nghttp2-prior-knowledge for 'nghttp', re-enable when ALPN works

   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/local/bin/nghttp", {"-d", testFile.string(), url});
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

using Args = std::vector<std::string>;

TEST_P(External, curl)
{
   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", fmt::format("@{}", testFile.string()), url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   auto future = spawn("/usr/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_many)
{
   std::vector<std::future<std::string>> futures;
   futures.reserve(10);

   for (size_t i = 0; i < futures.capacity(); ++i)
   {
      auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
      Args args = {"-sS", "-v", "--data-binary", fmt::format("@{}", testFile.string()), url};

      if (GetParam() == anyhttp::Protocol::h2)
         args.insert(args.begin(), "--http2-prior-knowledge");

      futures.emplace_back(spawn("/usr/bin/curl", std::move(args)));
   }

   run();

   for (auto& future : futures)
      EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_https)
{
   auto url = fmt::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "-k", "--data-binary", fmt::format("@{}", testFile.string()), url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2");
   else
      args.insert(args.begin(), "--http1.1"); // not implemented, yet

   auto future = spawn("/usr/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_multiple)
{
   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", fmt::format("@{}", testFile.string()), url, url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   // https://github.com/curl/curl/issues/10634 --> use custom built curl
   auto future = spawn("/usr/local/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize * 2);
}

TEST_P(External, curl_multiple_https)
{
   auto url = fmt::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "-k", "--data-binary", fmt::format("@{}", testFile.string()),
                url,   url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2");
   else
      args.insert(args.begin(), "--http1.1");

   auto future = spawn("/usr/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize * 2);
}

TEST_P(External, h2spec)
{
   if (GetParam() != anyhttp::Protocol::h2)
      GTEST_SKIP();

   handler = h2spec;

   auto future = spawn("bin/h2spec", {"--host", server->local_endpoint().address().to_string(),
                                      "--port", std::to_string(server->local_endpoint().port()),
                                      "--path", "/custom", "--timeout", "1", "--verbose"});
   run();

   const std::string output = future.get();

   std::smatch match;
   std::regex regex(R"(((\d+) tests, (\d+) passed, (\d+) skipped, (\d+) failed))");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex));
   EXPECT_EQ(std::stoi(match[2].str()), 146) << match[1];

   // https://github.com/nghttp2/nghttp2/issues/2278
   // https://github.com/nghttp2/nghttp2/issues/2365
   const int expected_ok = NGHTTP2_VERSION_NUM >= 0x004100 ? 139 : 145;
   EXPECT_EQ(std::stoi(match[3].str()), expected_ok) << output;
}

TEST_P(External, h2load)
{
   const size_t n = 100;
   auto url = fmt::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-d", "64kminus1", "-n", std::to_string(n), "-c", "4", "-m", "3", url};

   if (GetParam() == anyhttp::Protocol::http11)
      args.insert(args.begin(), "--h1");

   auto future = spawn("/usr/local/bin/h2load", std::move(args));
   run();

   const std::string output = future.get();
   std::smatch match;
   std::regex regex(
      R"((\d+) total, \d+ started, (\d+) done, (\d+) succeeded, (\d+) failed, \d+ errored)");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex)) << output;
   EXPECT_EQ(std::stoul(match[3].str()), n) << match[1];
   EXPECT_EQ(std::stoul(match[4].str()), 0) << match[1];

   regex = std::regex(R"(\((\d+)\) data)");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex)) << output;
   EXPECT_EQ(std::stoul(match[1].str()), n * 65535) << match[1];
}

// -------------------------------------------------------------------------------------------------

TEST_P(External, echo)
{
   co_spawn(context.get_executor(), spawn_process("/usr/bin/echo", {}), detached);
   run();
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
#if defined(MULTITHREADED)
      client.emplace(make_strand(context.get_executor()), config);
#else
      client.emplace(context.get_executor(), config);
#endif
   }

protected:
   boost::urls::url url{"http://127.0.0.2"};
   std::optional<client::Client> client;
};

class ClientAsync : public Client
{
public:
   auto completion_handler()
   {
      return [this](const std::exception_ptr& ex)
      {
         if (ex)
            logw("client finished with {}", what(ex));
         logi("client finished, resetting server");
         server.reset();
         work.reset();
      };
   }

   void SetUp() override
   {
      Client::SetUp();

      //
      // Spawn the testcase coroutine on the client's strand so that access to it is serialized.
      //
      co_spawn(
         client->executor(),
         [&]() -> awaitable<void>
         {
            auto session = co_await client->async_connect(asio::deferred);
            try
            {
               logd("running test...");
               co_await test(std::move(session));
               logd("running test... done");
            }
            catch (const boost::system::system_error& ex)
            {
               logd("running test: {}", ex.what());
               throw;
            }
         },
         completion_handler());
   }

public:
   decltype(boost::asio::make_work_guard(context)) work = boost::asio::make_work_guard(context);
   std::function<awaitable<void>(Session session)> test;
};

INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2),
                         NameGenerator);

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
   run();
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
   run();
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("unknown"), {}, deferred);
      co_await send(request, 1024 * 1024);
      auto response = co_await request.async_get_response(asio::deferred);
      auto received = co_await receive(response);
   };
   run();
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
   run();
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
   run();
}

TEST_P(ClientAsync, WHEN_invalid_port_in_host_header_THEN_reports_error)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request =
         co_await session.async_submit(url.set_path("echo"), {{"Host", "host:12345x"}}, deferred);
      auto response = co_await (sendEOF(request) && read_response(request));
   };
   run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, ServerYieldFirst)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await yield(10);
      co_await response.async_submit(200, {}, deferred);
      co_await yield(10);
      co_await response.async_write({}, deferred);
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {}, deferred);
      co_await request.async_write({}, deferred);
      co_await (read_response(request) || sleep(2s));
   };
   run();
}

TEST_P(ClientAsync, Custom)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {}, deferred);
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         size_t n = co_await request.async_read_some(asio::buffer(buffer), deferred);
         co_await response.async_write(asio::buffer(buffer, n), deferred);
         if (n == 0)
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
   run();
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
      auto request =
         co_await session.async_submit(url.set_path("custom"), {{"content-length", "0"}}, deferred);
      auto res = co_await (send(request, 0) && read_response(request));
   };
   run();
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   handler = [&](server::Request request, server::Response response) -> awaitable<void>
   { co_return; };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {}, deferred);
      auto res = co_await (send(request, 0) && try_read_response(request));
      std::cout << res.error().message() << std::endl;
   };
   run();
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
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
      auto received = co_await (std::move(sender) && receive(response));
      loge("received: {}", received);
      EXPECT_GT(received, 0);
   };
   run();
}

TEST_P(ClientAsync, PostRangeImmediate)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
      auto received = co_await (std::move(sender) && read_response(request));
      loge("received: {}", received);
      EXPECT_GT(received, 0);
   };
   run();
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
   run();
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
   run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Backpressure)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      auto sender = send(request, rv::iota(uint8_t(0)));
      co_await (std::move(sender) || sleep(2s));
      co_await sendEOF(request);
      // co_await sleep(500ms);
      // auto buf = co_await response.async_read_some(as_tuple(asio::deferred));
      // co_await sleep(500ms);
      // auto received = co_await (sendEOF(request) && try_receive(response));
      fmt::println("receiving....");
      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      fmt::println("receiving... done, got {} bytes ({})", received, ec.message());
      EXPECT_GT(received, 0);
      // FIXME: we should be able to receive the remainders that already have been buffered
   };
   run();
}

//
// Cancellation of a large buffer with Content-Length.
//
// Any short write of a body with known content length should result in a 'partial message' error.
//
TEST_P(ClientAsync, CancellationContentLength)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 0; i <= 20; ++i)
      {
         const size_t length = 5ul * 1024 * 1024;
         auto request = co_await session.async_submit(
            url.set_path("echo"), {{"content-length", std::to_string(length)}}, deferred);
         auto response = co_await request.async_get_response(asio::deferred);
         std::vector<char> buffer(length);
         auto sender = sendAndForceEOF(request, std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         fmt::println("received {} bytes ({}, yielded {})", std::get<1>(received), ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);
      }
   };
   run();
}

//
// Cancellation of sending a single, large buffer without Content-Length.
//
// HTTP/1.1: As always when not providing Content-Length, the data is chunked. When sending data
//           as a single, large buffer, this will result in a single, large chunk of same size.
//           If sending that chunk is cancelled, there is no way to recover. The receiver will
//           close the connection in this situation.
//
// HTTP/2: Cancelling a large buffer without Content-Length will look to the server just like a
//         small buffer. No error is raised. FIXME: we could try to support cancellation here
//         by closing the stream without sending an EOF.
//
TEST_P(ClientAsync, Cancellation)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 0; i <= 20; ++i)
      {
         const size_t length = 5ul * 1024 * 1024;
         auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
         auto response = co_await request.async_get_response(asio::deferred);
         std::vector<char> buffer(length, 'a');
         auto sender = sendAndForceEOF(request, std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         fmt::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);
      }
   };
   run();
}

//
// Cancellation of sending a large amount of data that is split into many smaller chunks.
//
// This should work with any protocol, without error. As we don't give a Content-Length in advance,
// cancelling the upload should not be terminal. BUT: cancellation of a parallel group seems to
// do 'terminal' cancellation...
//
TEST_P(ClientAsync, CancellationRange)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 6; i <= 7; ++i)
      {
         co_await yield();
         auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
         auto response = co_await request.async_get_response(asio::deferred);
         auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         fmt::println("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);
         co_await client->async_connect(deferred);
      }
   };
   run();
}

TEST_P(ClientAsync, PerOperationCancellation)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);

      asio::cancellation_signal cancel;

      // auto buf = co_await response.async_read_some(deferred);
      // auto buf = co_await (response.async_read_some(deferred) || sleep(10ms));
      asio::steady_timer timer(co_await asio::this_coro::executor, 110ms);
      timer.async_wait([&](const boost::system::error_code& ec)
                       { cancel.emit(asio::cancellation_type::terminal); });
      std::array<uint8_t, 1024> buffer;
      size_t n = co_await response.async_read_some(
         asio::buffer(buffer), asio::bind_cancellation_slot(cancel.slot(), deferred));
      std::println("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
      co_await yield();
      std::println("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
   };
   run();
}

//
// Send more than content length allows.
//
TEST_P(ClientAsync, SendMoreThanContentLength)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("eat_request"),
                                                   {{"content-length", "1024"}}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      co_await receive(response);
      co_await send(request, rv::iota(uint8_t(0)) | rv::take(10 * 1024 + 1));
   };
   run();
}

// =================================================================================================

TEST_P(ClientAsync, ClientDropRequest)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
   };
   run();
}

// =================================================================================================

TEST_P(ClientAsync, ResetServerDuringRequest)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);

      // co_spawn(context, send(request, rv::iota(uint8_t(0))), detached);
      std::future<std::exception_ptr> future;
      co_spawn(request.executor(), send(request, rv::iota(uint8_t(0))), error_future(future));

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

      auto exception_ptr = future.get();

      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      loge("received: {} ({} bytes)", ec.message(), received);
      // future.wait_for(2s);
   };
   run();
}

TEST_P(ClientAsync, SpawnAndForget)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      co_await yield();

      std::println("- - spawning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - ");
      co_spawn(
         context,
         [request = std::move(request)]() mutable -> awaitable<void>
         { //
            std::println("- - SPAWNED - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
            co_await yield();
            std::println("- - SPAWNED, sending  - - - - - - - - - - - - - - - - - - - - - - - - -");
            co_await send(request, rv::iota(uint8_t(0)));
         },
         detached);
   };
   run();
}

// =================================================================================================
