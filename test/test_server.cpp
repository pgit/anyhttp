#include "anyhttp/client.hpp"
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>

#include <boost/algorithm/string/join.hpp>

#include <boost/filesystem/path.hpp>

#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <nghttp2/nghttp2ver.h>

#include <future>
#include <random>
#include <ranges>
#include <regex>

using namespace std::string_view_literals;
using namespace std::chrono_literals;
namespace bp = boost::process::v2;

namespace asio = boost::asio;
using namespace asio;
using namespace asio::experimental::awaitable_operators;
using tcp = ip::tcp;

namespace rv = std::ranges::views;

using namespace anyhttp;

// =================================================================================================

std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
};

// =================================================================================================

class Empty : public testing::Test
{
};

TEST_F(Empty, Hello)
{
   boost::process::filesystem::path path{"."};
   LOG("Hello, World!");
   LOG("Path: {}", path.string());
}

TEST_F(Empty, Path)
{
   bp::filesystem::path path("/usr/bin/echo");
   LOG("spawn: {}", path.string());
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
   client.async_connect([&](boost::system::error_code ec, Session session)
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
   client.async_connect([&](boost::system::error_code ec, Session session)
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
            co_await echo(std::move(request), std::move(response));
         else if (request.url().path() == "/echo_delayed")
         {
            co_await sleep(1s);
            co_return co_await echo(std::move(request), std::move(response));
         }
         else if (request.url().path() == "/eat_request")
            co_await eat_request(std::move(request), std::move(response));
         else if (request.url().path() == "/discard")
            co_return;
         else if (request.url().path() == "/h2spec")
            co_await h2spec(std::move(request), std::move(response));
         else if (request.url().path() == "/detach")
            co_spawn(server->get_executor(), detach(std::move(request), std::move(response)),
                     [&](const std::exception_ptr&) { logi("client finished"); });
         else if (request.url().path() == "/custom")
            co_await custom(std::move(request), std::move(response));
         else
            co_await not_found(std::move(request), std::move(response));
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
   std::function<awaitable<void>(server::Request request, server::Response response)> custom;
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
   auto split_lines(std::string_view lines)
   {
      if (lines.ends_with('\n'))
         lines.remove_suffix(1);

      return lines | std::views::split('\n') |
             std::views::transform([](auto range) { return std::string_view(range); });
   }

   awaitable<void> log(std::string prefix, readable_pipe& pipe)
   {
      std::string buffer;
      auto print = [&](std::string_view line)
      {
         if (line.ends_with('\r'))
            line.remove_suffix(1);

         // print trailing '…' if there is more data in the buffer after this line
         const auto continuation = (line.size() + 1 == buffer.size()) ? "" : "…";
         LOG("{}: \x1b[32m{}\x1b[0m{}", prefix, line, continuation);
      };

      auto cs = co_await this_coro::cancellation_state;
      try
      {
         for (;;)
         {
            auto n = co_await async_read_until(pipe, dynamic_buffer(buffer), '\n');
            print(std::string_view(buffer).substr(0, n - 1));
            buffer.erase(0, n);
         }
      }
      catch (const boost::system::system_error& ec)
      {
         if (cs.cancelled() != cancellation_type::none)
            LOG("CANCELLED");

         for (auto line : split_lines(buffer))
            print(line);

         if (ec.code() == error::eof)
            co_return;

         LOG("{}: {}", prefix, ec.code().message());
         throw;
      }
   }

   awaitable<std::string> consume(readable_pipe pipe)
   {
      std::string result;
      std::vector<char> buf(1460);
      for (;;)
      {
         auto [ec, nread] = co_await asio::async_read(pipe, asio::buffer(buf), as_tuple);
         result += std::string_view(buf.data(), nread);
         if (nread)
            logi("STDOUT: {} bytes", nread);

         if (ec == error::eof)
            co_return result;

         if (ec)
         {
            LOG("consume: {}", ec.message());
            break;
         }
      }
      co_return result;
   }

   awaitable<std::string> spawn_process(bp::filesystem::path path, std::vector<std::string> args)
   {
      logi("spawn: {} {}", path.generic_string(), boost::algorithm::join(args, " "));

      readable_pipe out(context), err(context);
      bp::process child(co_await this_coro::executor, path, args,
                        bp::process_stdio{.out = out, .err = err},
                        bp::process_environment{{"LD_LIBRARY_PATH=/usr/local/lib"}});

      logi("spawn: starting to communicate...");
#if 1
      auto result = co_await (log("STDERR", err) && consume(std::move(out)));
#else
      co_await (log("STDERR", err) && log("STDOUT", out));
      auto result = std::string();
#endif
      logi("spawn: starting to communicate... done");

      co_await child.async_wait();
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

   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/local/bin/nghttp", {"-d", testFile.string(), url});
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

using Args = std::vector<std::string>;

TEST_P(External, curl)
{
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", std::format("@{}", testFile.string()), url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   auto future = spawn("/usr/local/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_many)
{
   std::vector<std::future<std::string>> futures;
   futures.reserve(10);

   for (size_t i = 0; i < futures.capacity(); ++i)
   {
      auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
      Args args = {"-sS", "-v", "--data-binary", std::format("@{}", testFile.string()), url};

      if (GetParam() == anyhttp::Protocol::h2)
         args.insert(args.begin(), "--http2-prior-knowledge");

      futures.emplace_back(spawn("/usr/local/bin/curl", std::move(args)));
   }

   run();

   for (auto& future : futures)
      EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_https)
{
   auto url = std::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "-k", "--data-binary", std::format("@{}", testFile.string()), url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2");
   else
      args.insert(args.begin(), "--http1.1"); // not implemented, yet

   auto future = spawn("/usr/local/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_P(External, curl_multiple)
{
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", std::format("@{}", testFile.string()), url, url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   // https://github.com/curl/curl/issues/10634 --> use custom built curl
   auto future = spawn("/usr/local/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize * 2);
}

TEST_P(External, curl_multiple_https)
{
   auto url = std::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "-k", "--data-binary", std::format("@{}", testFile.string()),
                url,   url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2");
   else
      args.insert(args.begin(), "--http1.1");

   auto future = spawn("/usr/local/bin/curl", std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize * 2);
}

TEST_P(External, nc_crazy_chunked)
{
   if (GetParam() == anyhttp::Protocol::h2)
      GTEST_SKIP();

   auto cmd =
      std::format("nc 127.0.0.2 {} <test/data/crazy-chunked.txt", server->local_endpoint().port());
   auto future = spawn("/usr/bin/bash", {"-c", cmd});
   run();

   auto out = future.get();
   EXPECT_GT(out.size(), 0);
   EXPECT_TRUE(out.contains("Hello, World!\n"));
}

TEST_P(External, h2spec)
{
   if (GetParam() != anyhttp::Protocol::h2)
      GTEST_SKIP();

   auto future = spawn("bin/h2spec", {"--host", server->local_endpoint().address().to_string(),
                                      "--port", std::to_string(server->local_endpoint().port()),
                                      "--path", "/h2spec", "--timeout", "1", "--verbose"});
   run();

   const std::string output = future.get();

   std::smatch match;
   std::regex regex(R"(((\d+) tests, (\d+) passed, (\d+) skipped, (\d+) failed))");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex));
   EXPECT_EQ(std::stoi(match[2].str()), 146) << match[1];

   // https://github.com/nghttp2/nghttp2/issues/2278
   // https://github.com/nghttp2/nghttp2/issues/2365
   const int expected_ok = std::invoke([]
   {
      if (NGHTTP2_VERSION_NUM >= 0x004200) // 1.66
         return 138; // 6.9.1
      else if (NGHTTP2_VERSION_NUM == 0x004100) // 1.65
         return 139;
      else
         return 145;
   });
   EXPECT_EQ(std::stoi(match[3].str()), expected_ok) << output;
}

TEST_P(External, h2load)
{
   const size_t n = 100;
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-d", "test/data/64kminus1", "-n", std::to_string(n), "-c", "4", "-m", "3", url};

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
   boost::urls::url url{"http://127.0.0.2/custom"};
   std::optional<client::Client> client;
};

// -------------------------------------------------------------------------------------------------

class ClientAsync : public Client
{
public:
   auto completion_handler()
   {
      return [this](const std::exception_ptr& ex)
      {
         if (ex)
            logw("client finished with {}, stopping server", what(ex));
         else
            logi("client finished, stopping server");
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
      co_spawn(client->get_executor(), [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect();
         try
         {
            logd("running test...");
            co_await test(std::move(session));
            logd("running test... done");
         }
         catch (const boost::system::system_error& ex)
         {
            logd("running test: {}", ex.code().message());
            throw;
         }
      }, completion_handler());
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
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
      auto request = co_await session.async_submit(url.set_path(""), {});
      co_await send(request, 1024);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 404);
      auto received = co_await receive(response);
   };
   run();
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("unknown"), {});
      co_await send(request, 1024 * 1024);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 404);
      auto received = co_await receive(response);
   };
   run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_error_500)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("discard"), {});
      co_await send(request, 1024);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 500);
      auto received = co_await receive(response);
   };
   run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_delayed_THEN_error_500)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      co_await send(request, 1024);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 500);
      EXPECT_THROW(co_await receive(response), boost::system::system_error);
   };
   run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_with_body_delayed_THEN_error_500)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      co_await send(request, rv::iota(0));
      co_await (yield(5));
      auto response = co_await request.async_get_response();
      auto received = co_await receive(response);
   };
   run();
}

TEST_P(ClientAsync, WHEN_invalid_port_in_host_header_THEN_reports_error)
{
   test = [&](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("Host", "host:12345x");
      auto request = co_await session.async_submit(url.set_path("echo"), fields);
      auto response = co_await (sendEOF(request) && read_response(request));
   };
   run();
}

TEST_P(ClientAsync, WHEN_get_response_is_called_twice_THEN_reports_error)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto response = co_await request.async_get_response();
      EXPECT_THROW(co_await request.async_get_response(), boost::system::system_error);
   };
   run();
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_is_still_able_to_deliver_response)
{
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await sleep(150ms);
      request.reset();
      co_await response.async_submit(200, {});
      co_await response.async_write({});
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      auto executor = co_await this_coro::executor;
      // auto [ec] = co_await co_spawn(executor, send(request, rv::iota(uint8_t(0))), as_tuple);
      EXPECT_THROW(co_await send(request, rv::iota(uint8_t(0))), boost::system::system_error);
      auto response = co_await request.async_get_response();
   };
   run();
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
   std::uniform_int_distribution<> dist(0, 10);

   bool done = false;
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await yield(dist(gen));
      Fields fields;
      fields.set("Content-Length", "15");
      co_await response.async_submit(200, fields);
      co_await yield(dist(gen));
      co_await response.async_write(asio::buffer("Hello, Client!"sv));
      co_await yield(dist(gen));
      co_await response.async_write({});
      co_await yield(dist(gen));
   };
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 0; i < 100; ++i)
      {
         co_await yield(dist(gen));
         Fields fields;
         if (GetParam() == anyhttp::Protocol::http11)
            fields.set("Connection", "Keep-Alive");
         fields.set("Content-Length", "0");
         auto request = co_await session.async_submit(url, fields);
         co_await yield(dist(gen));
         co_await request.async_write({});
         co_await yield(dist(gen));
         co_await read_response(request);
      }
      done = true;
   };
   run();
   EXPECT_TRUE(done);
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, ServerYieldFirst)
{
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await yield(10);
      co_await response.async_submit(200, {});
      co_await yield(10);
      co_await response.async_write({});
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({});
      co_await (read_response(request) || sleep(2s));
   };
   run();
}

TEST_P(ClientAsync, Custom)
{
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      std::array<uint8_t, 1024> buffer;
      for (;;)
      {
         size_t n = co_await request.async_read_some(asio::buffer(buffer));
         co_await response.async_write(asio::buffer(buffer, n));
         if (n == 0)
            co_return;
      }
   };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {});
      size_t bytes = 1024;
      auto res = co_await (send(request, bytes) && read_response(request));
      EXPECT_EQ(bytes, res);
   };
   run();
}

TEST_P(ClientAsync, IgnoreRequest)
{
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      co_await response.async_write({});
   };
   test = [&](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("content-length", "0");
      auto request = co_await session.async_submit(url.set_path("custom"), fields);
      auto res = co_await (send(request, 0) && read_response(request));
   };
   run();
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   custom = [&](server::Request request, server::Response response) -> awaitable<void>
   { co_return; };
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("custom"), {});
      auto res = co_await (send(request, 0) && try_read_response(request));
      LOG("{}", res.error().message());
   };
   run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, PostRange)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      // co_await request.async_write(asio::buffer("ping"sv)); // FIXME:
      auto response = co_await request.async_get_response();
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      constexpr size_t bytes = 1024;
      co_await send(request, bytes);
      EXPECT_EQ(co_await receive(response), bytes);
   };
   run();
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
   test = [&](Session session) -> awaitable<void>
   {
      auto request1 = co_await session.async_submit(url.set_path("echo"), {});      
      co_await request1.async_write(asio::buffer("Hello, Server #1!"sv));
      co_await request1.async_write({});

      auto request2 = co_await session.async_submit(url.set_path("echo"), {});
      co_await request2.async_write(asio::buffer("Hello, Server #2! XYZ"sv));
      co_await request2.async_write({});

      auto response1 = co_await request1.async_get_response();
      EXPECT_EQ(co_await receive(response1), 17);

      auto response2 = co_await request2.async_get_response();
      EXPECT_EQ(co_await receive(response2), 21);
   };
   run();
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, EatRequest)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("eat_request"), {});
      co_await send(request, 1024);
      auto response = co_await request.async_get_response();
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      auto sender = send(request, rv::iota(uint8_t(0)));
      co_await (std::move(sender) || sleep(2s));
      // FIXME: count bytes sent, just like asio::async_write() does
      // FIXME: or even use asio::async_write() on top of a async_write_some() implementation

      // Now that the flow control window is 0, we can't even send an EOF any more:
      // co_await sendEOF(request);

      // So instead, we start doing this in background, to be resumed as soon as the window reopens.
      co_spawn(co_await this_coro::executor, sendEOF(request), detached); // FIXME: join

      LOG("receiving....");
      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      LOG("receiving... done, got {} bytes ({})", received, ec.message());
      EXPECT_GT(received, 0);
      // FIXME: we should be able to receive the remainders that already have been buffered
      // FIXME: in the end, this must be the same as the the bytes sent above
   };
   run();
}

//
// Cancellation of a large buffer with Content-Length.
//
// Any short write of a body with known content length should result in a 'partial message' error.
//
// FIXME: With nghttp2 1.67, the partial message results in a GOAWAY, so that only one request
//        can be made. The following request should throw an exception.
//
TEST_P(ClientAsync, CancellationContentLength)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 0; i <= 20; ++i)
      {
         const size_t length = 5ul * 1024 * 1024;
         Fields fields;
         fields.set("content-length", std::to_string(length));
         auto request = co_await session.async_submit(url.set_path("echo"), fields);
         auto response = co_await request.async_get_response();
         std::vector<char> buffer(length);
         auto sender = sendAndForceEOF(request, std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         LOG("received {} bytes ({}, yielded {})", std::get<1>(received), ec.message(), i);
         EXPECT_LT(std::get<1>(received), length);
         EXPECT_EQ(ec, boost::beast::http::error::partial_message);
         co_await yield(5);
      }
   };
   run();
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
TEST_P(ClientAsync, Cancellation)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 0; i <= 20; ++i)
      {
         const size_t length = 5ul * 1024 * 1024;
         auto request = co_await session.async_submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response();
         std::vector<char> buffer(length, 'a');
         // auto sender = sendAndForceEOF(request, std::string_view(buffer));
         auto sender = sendAndDrop(std::move(request), std::string_view(buffer));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         LOG("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
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
// TODO: Aside using operator||, when manually setting up a parallel group, it is possible to
//       specify the cancellation type that should be used.
//
// TODO: If an operation supports "partial" as well, it is free to cancel like that even when
//       requested to do terminal "cancellation". Cancellation types are backward compatible this
//       way.
//
TEST_P(ClientAsync, CancellationRange)
{
   test = [&](Session session) -> awaitable<void>
   {
      for (size_t i = 6; i <= 7; ++i)
      {
         co_await yield();
         auto request = co_await session.async_submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response();
         // auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)));
         auto sender = sendAndDrop(std::move(request), rv::iota(uint8_t(0)));

         boost::system::error_code ec;
         auto received = co_await ((std::move(sender) || yield(i)) && try_receive(response, ec));
         LOG("received {} bytes ({}, yield {})", std::get<1>(received), ec.message(), i);
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      asio::cancellation_signal cancel;

      // auto buf = co_await response.async_read_some(deferred);
      // auto buf = co_await (response.async_read_some(deferred) || sleep(10ms));
      asio::steady_timer timer(co_await asio::this_coro::executor, 110ms);
      timer.async_wait([&](const boost::system::error_code& ec)
      { cancel.emit(asio::cancellation_type::terminal); });
      std::array<uint8_t, 1024> buffer;
      size_t n = co_await response.async_read_some(
         asio::buffer(buffer), asio::bind_cancellation_slot(cancel.slot(), deferred));
      LOG("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
      co_await yield();
      LOG("- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
   };
   run();
}

TEST_P(ClientAsync, CancelAfter)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo_delayed"), {});
      auto [ec, response] = co_await request.async_get_response(cancel_after(250ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);

      std::tie(ec, response) = co_await request.async_get_response(cancel_after(0ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);

      std::tie(ec, response) = co_await request.async_get_response(as_tuple);
      EXPECT_FALSE(ec);

      co_await request.async_write(asio::buffer("Hello, Client!"sv));
      co_await request.async_write({});
      auto received = co_await receive(response);
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
      Fields fields;
      fields.set("content-length", "1024");
      auto request = co_await session.async_submit(url.set_path("eat_request"), fields);
      auto response = co_await request.async_get_response();
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
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
   };
   run();
}

// =================================================================================================

TEST_P(ClientAsync, ResetServerDuringRequest)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      auto future = co_spawn(request.get_executor(), send(request, rv::iota(uint8_t(0))),
                             use_future(as_tuple));

      LOG("=============================================================================");
      for (size_t i = 0; i < 10; ++i)
      {
         LOG("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -", i);
         co_await yield();
      }

      LOG("=============================================================================");
      server.reset();

      for (size_t i = 0; i < 10; ++i)
      {
         LOG("- - {} - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -", i);
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
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // FIXME: ASAN errors

   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      co_await yield();

      LOG("- - spawning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - ");
      co_spawn(context,
               [request = std::move(request)]() mutable -> awaitable<void>
      { //
         LOG("- - SPAWNED - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await yield(5);
         LOG("- - SPAWNED, sending  - - - - - - - - - - - - - - - - - - - - - - - - -");
         co_await send(request, rv::iota(uint8_t(0)));
      }, detached);
   };
   run();
}

// =================================================================================================
