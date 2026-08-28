#include "anyhttp/client.hpp"
#include "anyhttp/file_handler.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_immediate_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/promise.hpp>
#include <boost/asio/experimental/use_promise.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>

#include <boost/algorithm/string/join.hpp>

#include <boost/lexical_cast.hpp>
#include <boost/lexical_cast/bad_lexical_cast.hpp>

#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <boost/scope/scope_exit.hpp>
#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <boost/url/url.hpp>

#include <nghttp2/nghttp2ver.h>

#include <fstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <pthread.h>

#include <chrono>
#include <cstddef>
#include <filesystem>

#include <future>
#include <print>
#include <random>
#include <ranges>
#include <regex>
#include <spdlog/common.h>
#include <thread>

using namespace std::string_view_literals;
using namespace std::chrono_literals;
namespace bp = boost::process::v2;

namespace asio = boost::asio;
using namespace asio;
using namespace asio::experimental::awaitable_operators;
using tcp = ip::tcp;

namespace rv = std::ranges::views;

using namespace anyhttp;

// https://github.com/curl/curl/issues/10634 --> use custom built curl
#define CURL_PATH "/usr/local/bin/curl"
#define NGHTTP_PATH "/usr/local/bin/nghttp"
#define H2LOAD_PATH "/usr/local/bin/h2load"

// =================================================================================================

/// Returns HTTP11 or HTTP/2 depending on the protocol.
static std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
};

static void setupLogging()
{
#if defined(GITHUB_ACTIONS)
   spdlog::set_level(spdlog::level::warn);
#elif defined(NDEBUG)
   spdlog::set_level(spdlog::level::info);
#else
   spdlog::set_level(spdlog::level::debug);
#endif
}

// =================================================================================================

class ClientConnect : public testing::Test
{
public:
   void SetUp() override { setupLogging(); }
};

TEST_F(ClientConnect, WHEN_unknown_host_THEN_completes_with_host_not_found_eventually)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://this-domain-does-not-exist:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_TRUE(ec == boost::asio::error::netdb_errors::host_not_found ||
                  ec == boost::asio::error::netdb_errors::host_not_found_try_again);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_wrong_port_THEN_completes_with_host_not_found_eventually)
{
   boost::asio::io_context context;
   auto port = get_unused_port(context);
   client::Config config{.url = boost::urls::url("http://localhost").set_port_number(port)};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::connection_refused);
   });
   context.run();
}

TEST_F(ClientConnect, WHEN_async_connect_is_cancelled_THEN_returns_operation_aborted)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://localhost:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect(cancel_after(0ms, [this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
   }));

   context.run();
}

TEST_F(ClientConnect, WHEN_connect_to_broadcast_ip_THEN_completes_with_network_unreachable)
{
   boost::asio::io_context context;
   client::Config config{.url = boost::urls::url("http://255.255.255.255:12345")};
   client::Client client(context.get_executor(), config);
   client.async_connect([this](boost::system::error_code ec, Session session)
   {
      loge("ERROR: {}", ec.message());
      EXPECT_EQ(ec, boost::system::errc::network_unreachable);
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
   //
   // Number of threads run() will run the io_context on. More than one makes the server put
   // every connection on its own strand, see below.
   //
   virtual size_t threads() const
   {
#if defined(MULTITHREADED)
      return std::max(2u, std::thread::hardware_concurrency());
#else
      return 1;
#endif
   }

   void SetUp() override
   {
      setupLogging();

      auto config = server::Config{.listen_address = "127.0.0.2", .port = 0};
      config.use_strand = threads() > 1;

      //
      // The main server acceptor loop does not need to run on a strand. Instead, a per-connection
      // strand is created after accepting a new connection.
      //
      server.emplace(context.get_executor(), config);
      server->setRequestHandler(
         [this](server::Request request, server::Response response) -> awaitable<void>
      {
         logd("{} ({})", request.url().path(), request.url().buffer());

         auto url = request.url();
         auto params = url.params();
         if (auto it = params.find("delay"); it != params.end())
         {
            try
            {
               using ms = std::chrono::milliseconds;
               auto delay_ms = boost::lexical_cast<ms::rep>((*it).value);
               co_await sleep(ms{delay_ms});
            }
            catch (boost::bad_lexical_cast&)
            {
               loge("invalid number: {}", (*it).value);
            }
         }

         if (request.url().path() == "/echo")
            co_await echo(std::move(request), std::move(response));
         else if (request.url().path() == "/eat_request")
            co_await eat_request(std::move(request), std::move(response));
         else if (request.url().path() == "/discard")
            co_return;
         else if (request.url().path() == "/h2spec")
            co_await h2spec(std::move(request), std::move(response));
         else if (request.url().path() == "/dump")
            co_await dump(std::move(request), std::move(response));
         else if (request.url().path() == "/dump space")
            co_await dump(std::move(request), std::move(response));
         else if (request.url().path() == "/detach")
            co_await detach(std::move(request), std::move(response));
         else if (request.url().path().starts_with("/custom"))
            co_await custom(std::move(request), std::move(response));
         else
            co_await not_found(std::move(request), std::move(response));
      });
   }

   void run()
   {
      const size_t n = threads();
      if (n <= 1)
      {
         ::run(context);
         return;
      }

      //
      // The extra threads use context.run() directly: the per-operation logging of ::run() is
      // meant for single-threaded debugging and would just interleave into noise here.
      //
      auto pool = rv::iota(size_t{1}, n) | rv::transform([this](size_t) {
         return std::jthread([this] { context.run(); });
      }) | std::ranges::to<std::vector>();

      context.run();
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
         std::println("{}: \x1b[32m{}\x1b[0m{}", prefix, line, continuation);
      };

      auto cs = co_await this_coro::cancellation_state;
      try
      {
         for (;;)
         {
            auto n = co_await async_read_until(pipe, dynamic_buffer(buffer), '\n');
            for (;;)
            {
               print(std::string_view(buffer).substr(0, n - 1));
               buffer.erase(0, n);

               // try to bundle multiple lines, looks nicer in debug output
               auto pos = buffer.find('\n');
               if (pos == std::string::npos)
                  break;
               n = pos + 1;
            }
         }
      }
      catch (const boost::system::system_error& ec)
      {
         std::println("{}: {}", prefix, ec.code().message());
         if (cs.cancelled() != cancellation_type::none)
            std::println("{}: CANCELLED ({})", prefix, cs.cancelled());

         for (auto line : split_lines(buffer))
            print(line);

         if (ec.code() == error::eof)
            co_return;

         throw;
      }
   }

   awaitable<std::string> read_all(readable_pipe pipe)
   {
      std::string result;
      auto [ec, nread] = co_await asio::async_read(pipe, asio::dynamic_buffer(result), as_tuple);
      logi("STDOUT: {} bytes ({})", nread, what(ec));
      if (ec && ec != error::eof)
         throw boost::system::system_error(ec);
      co_return result;
   }

   awaitable<std::string> spawn_process(std::filesystem::path path, std::vector<std::string> args)
   {
      logi("spawn: {} {}", path.generic_string(), boost::algorithm::join(args, " "));

      auto ex = co_await this_coro::executor;
      readable_pipe out(ex), err(ex);
      bp::process child(ex, path, args, bp::process_stdio{.out = out, .err = err});
      // bp::process_environment{{"LD_LIBRARY_PATH=/usr/local/lib"}});

      logi("spawn: starting to communicate...");
#if 1
      auto result = co_await (log("STDERR", err) && read_all(std::move(out)));
#else
      co_await (log("STDERR", err) && log("STDOUT", out));
      auto result = std::string();
#endif
      logi("spawn: starting to communicate... done, read {} bytes", result.size());

      co_await child.async_wait();
      if (child.exit_code())
         logw("exit_code={}", child.exit_code());
      else
         logi("exit_code={}", child.exit_code());

      if (--numSpawned <= 0)
      {
         co_await post(server->get_executor());
         logi("all processes exited, stopping server...");
         server.reset();
         logi("all processes exited, stopping server... done");
      }

      co_return result;
   }

   std::future<std::string> spawn(std::filesystem::path path, std::vector<std::string> args)
   {
      ++numSpawned;
      std::promise<std::string> promise;
      auto future = promise.get_future();
      co_spawn(strand, spawn_process(std::move(path), std::move(args)),
               bind_executor(strand, [this, promise = std::move(promise)](
                                        const std::exception_ptr& ex, std::string str) mutable
      {
         if (ex)
         {
            loge("{}", what(ex));
            server.reset();
         }
         promise.set_value(std::move(str));
      }));
      return std::move(future);
   }

   //
   // Like spawn(CURL_PATH, args), but for Protocol::h3: QUIC handshakes can hang in ways
   // http11/h2 curl invocations don't, so wrap in a hard `timeout 5` safety net.
   //
   std::future<std::string> spawn_curl(std::vector<std::string> args)
   {
      if (GetParam() == anyhttp::Protocol::h3)
      {
         args.insert(args.begin(), {"5", CURL_PATH});
         return spawn("/usr/bin/timeout", std::move(args));
      }
      return spawn(CURL_PATH, std::move(args));
   }

   any_io_executor strand{make_strand(context.get_executor())};
   std::filesystem::path testFile{"CMakeLists.txt"};
   std::filesystem::path dataFile{"test/data/64kminus1"}; // posted by h2load, one file per request
   std::atomic<int> numSpawned = 0;
};

using Args = std::vector<std::string>;

// =================================================================================================

// plain-text only, so no HTTP/3
INSTANTIATE_TEST_SUITE_P(External, External,
                         ::testing::Values(anyhttp::Protocol::http11, // HTTP/1.1
                                           anyhttp::Protocol::h2), // HTTP/2
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(External, curl)
{
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", std::format("@{}", testFile.string()), url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   auto future = spawn(CURL_PATH, std::move(args));
   run();

   EXPECT_EQ(future.get().size(), file_size(testFile));
}

TEST_P(External, curl_multiple)
{
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   Args args = {"-sS", "-v", "--data-binary", std::format("@{}", testFile.string()), url, url};

   if (GetParam() == anyhttp::Protocol::h2)
      args.insert(args.begin(), "--http2-prior-knowledge");

   auto future = spawn(CURL_PATH, std::move(args));
   run();

   EXPECT_EQ(future.get().size(), file_size(testFile) * 2);
}

// =================================================================================================

class ExternalTLS : public External
{
protected:
   std::string curlProtocolParam()
   {
      switch (GetParam())
      {
      case anyhttp::Protocol::http11:
         return "--http1.1";
      case anyhttp::Protocol::h2:
         return "--http2";
      case anyhttp::Protocol::h3:
         return "--http3-only";
      }
   }

   //
   // Run h2load against /echo, posting the contents of 'dataFile' with every request, and check
   // that all of it came back. h2load speaks the protocol of the fixture parameter.
   //
   void h2load(size_t n, size_t clients, size_t streams)
   {
      auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
      Args args = {"-d", dataFile.string(),       "-n", std::to_string(n), //
                   "-c", std::to_string(clients), "-m", std::to_string(streams), url};

      switch (GetParam())
      {
      case anyhttp::Protocol::http11:
         args.insert(args.begin(), "--h1");
         break;
      case anyhttp::Protocol::h3:
         args.insert(args.begin(), "--h3"); // h2load negotiates h3 itself, http:// URL is fine
         break;
      default:
         break; // h2load defaults to HTTP/2
      }

      auto future = spawn(H2LOAD_PATH, std::move(args));
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
      EXPECT_EQ(std::stoul(match[1].str()), n * file_size(dataFile)) << match[1];
   }
};

INSTANTIATE_TEST_SUITE_P(ExternalTLS, ExternalTLS,
                         ::testing::Values(anyhttp::Protocol::http11, // HTTP/1.1
                                           anyhttp::Protocol::h2, // HTTP/2
                                           anyhttp::Protocol::h3), // HTTP/3 (QUIC)
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(ExternalTLS, curl)
{
   auto url = std::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   // clang-format off
   Args args = {curlProtocolParam(), "-sS", "-v",
                "--cacert", "pki/out/root.pem",
                "--data-binary", std::format("@{}", testFile.string()),
                url};
   // clang-format off

   auto future = spawn_curl(std::move(args));
   run();

   EXPECT_EQ(future.get().size(), file_size(testFile));
}

TEST_P(ExternalTLS, curl_many)
{
   std::vector<std::future<std::string>> futures;
   futures.reserve(10);

   for (size_t i = 0; i < futures.capacity(); ++i)
   {
      auto url = std::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
      // clang-format off
      Args args = {curlProtocolParam(), "-sS", "-v",
                  "--cacert", "pki/out/root.pem",
                  "--data-binary", std::format("@{}", testFile.string()),
                  url};
      // clang-format off

      futures.emplace_back(spawn_curl(std::move(args)));
   }

   run();

   for (auto& future : futures)
      EXPECT_EQ(future.get().size(), file_size(testFile));
}

TEST_P(ExternalTLS, curl_multiple)
{
   auto url = std::format("https://127.0.0.2:{}/echo", server->local_endpoint().port());
   // clang-format off
   Args args = {curlProtocolParam(), "-sS", "-v",
                "--cacert", "pki/out/root.pem",
                "--data-binary", std::format("@{}", testFile.string()),
                url, url, url, url};
   // clang-format off

   auto future = spawn_curl(std::move(args));
   run();

   EXPECT_EQ(future.get().size(), file_size(testFile) * 4);
}

// -------------------------------------------------------------------------------------------------

TEST_P(ExternalTLS, h2load) { h2load(100, 4, 3); }

// =================================================================================================

//
// Same as ExternalTLS, but with the io_context run on multiple threads, so every connection gets
// its own strand. For HTTP/3 this is the regression test for concurrent access to a single
// ngtcp2_conn, which used to crash right away.
//
class ExternalTLSThreaded : public ExternalTLS
{
protected:
   size_t threads() const override { return 8; }
};

INSTANTIATE_TEST_SUITE_P(ExternalTLSThreaded, ExternalTLSThreaded,
                         ::testing::Values(anyhttp::Protocol::http11, // HTTP/1.1
                                           anyhttp::Protocol::h2, // HTTP/2
                                           anyhttp::Protocol::h3), // HTTP/3 (QUIC)
                         NameGenerator);

TEST_P(ExternalTLSThreaded, h2load) { h2load(1000, 8, 5); }

// =================================================================================================

//
// Non-parametrized fixture for external tests that are tied to a specific protocol.
//
class ExternalCustom : public External
{
};

// -------------------------------------------------------------------------------------------------

TEST_F(ExternalCustom, netcat_crazy_chunked)
{
   auto cmd =
      std::format("nc 127.0.0.2 {} <test/data/crazy-chunked.txt", server->local_endpoint().port());
   auto future = spawn("/usr/bin/bash", {"-c", cmd});
   run();

   auto out = future.get();
   EXPECT_GT(out.size(), 0);
   EXPECT_TRUE(out.contains("Hello, World!\n"));
}

TEST_F(ExternalCustom, nghttp2)
{
   auto url = std::format("http://127.0.0.2:{}/echo", server->local_endpoint().port());
   auto future = spawn(NGHTTP_PATH, {"-d", testFile.string(), url});
   run();

   EXPECT_EQ(future.get().size(), file_size(testFile));
}

TEST_F(ExternalCustom, h2spec)
{
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

// =================================================================================================

class Client : public Server
{
protected:
   void SetUp() override
   {
      Server::SetUp();
      url.set_port_number(server->local_endpoint().port());
      client::Config config{.url = url, .protocol = GetParam()};
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
   auto token()
   {
      return [this](const std::exception_ptr& ep)
      {
         auto ec = code(ep);
         if (ec)
            logw("client completed with \x1b[1;31m{}\x1b[0m", what(ec));
         else
            logi("client completed successfully");

         on_complete(ec);

         logd("stopping server");
         server.reset();
         work.reset();
      };
   }

   MOCK_METHOD(void, on_complete, (boost::system::error_code ec), ());
   static constexpr auto Success = boost::system::error_code{};

   void SetUp() override
   {
      Client::SetUp();

      //
      // Spawn the testcase coroutine on the client's executor so that access to it is serialized.
      //
      co_spawn(client->get_executor(), [this]() -> awaitable<void>
      {
         if (test)
         {
            auto session = co_await client->async_connect();
            co_await test(std::move(session));
         }
      }, token());
   }

   void TearDown() override
   {
      EXPECT_CALL(*this, on_complete(boost::system::error_code{}));
      run();
   }

public:
   decltype(boost::asio::make_work_guard(context)) work = boost::asio::make_work_guard(context);
   std::function<awaitable<void>(Session session)> test;
};

INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                           anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_post_data_THEN_receive_echo)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      size_t bytes = 1024; //  * 1024 * 1024;
      auto count = co_await (send(request, bytes) && read_response(request));
      EXPECT_EQ(bytes, count);
   };
}

TEST_P(ClientAsync, WHEN_post_without_path_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path(""), {});
      co_await send(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_post_to_unknown_path_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("unknown"), {});
      co_await send(request, 1024 * 1024);
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 404);
      auto received = co_await count(response);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("discard"), {});
      co_await send(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_delayed_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      co_await send(request, 1024);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_TRUE(ec);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_with_body_delayed_THEN_error_500)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto executor = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("detach"), {});
      auto [ep] = co_await co_spawn(executor, send(request, rv::iota(uint8_t{0})), as_tuple);
      EXPECT_TRUE(ep);
   };
}

TEST_P(ClientAsync, WHEN_invalid_port_in_host_header_THEN_reports_error)
{
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("Host", "host:12345x");
      auto request = co_await session.async_submit(url.set_path("echo"), fields);
      auto response = co_await (send_eof(request) && read_response(request));
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_called_twice_THEN_reports_error)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::system::errc::success);
      std::tie(ec, response) = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::system::errc::connection_already_in_progress);
      EXPECT_EQ(ec, asio::error::basic_errors::already_started);
   };
}

TEST_P(ClientAsync, WHEN_get_response_is_detached_THEN_does_not_crash)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP();

   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"));
      request.async_get_response(detached);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_while_writing_THEN_connection_is_reset)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await sleep(150ms);
      request.reset();
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      auto executor = co_await this_coro::executor;
      auto [ec] = co_await co_spawn(executor, send(request, rv::iota(uint8_t(0))), as_tuple);
      EXPECT_EQ(code(ec), boost::system::errc::connection_reset);
   };
}

TEST_P(ClientAsync, WHEN_server_discards_request_and_response_THEN_completes_anyway)
{
   // if (GetParam() == anyhttp::Protocol::http11)
   //    GTEST_SKIP(); // FIXME: timeout

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      auto [ec, _] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::beast::http::error::end_of_stream);
      // EXPECT_EQ(ec, std::errc::connection_reset);
   };
}

TEST_P(ClientAsync, WHEN_client_cancels_write_THEN_can_resume)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects

   test = [this](Session session) -> awaitable<void>
   {
      co_await this_coro::throw_if_cancelled(false);
      auto executor = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("echo"));
      auto response = co_await request.async_get_response();

      // send as much data as possible within 1s, should run into backpressure
      auto [ep] = co_await co_spawn(executor, send(request, rv::iota(uint8_t(0))),
                                    cancel_after(1s, as_tuple));
      EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);

      if (GetParam() == anyhttp::Protocol::h3)
      {
         //
         // QUIC: whether the FIN can slip out while the send window is closed depends on flow
         // control timing, so don't assert either way here. What matters is that ending the
         // upload and draining the response together complete the exchange.
         //
         auto received = co_await (send_eof(request) && count(response));
         EXPECT_GT(received, 0);
      }
      else
      {
         // now, with a closed window, we cannot even end the upload
         std::tie(ep) = co_await co_spawn(executor, send_eof(request), cancel_after(1ms, as_tuple));
         EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);

         // as we have no control over when the send window is re-opened, wait for it in parallel
         auto received = co_await (send_eof(request) && count(response));
         EXPECT_GT(received, 0);
      }
   };
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

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::uniform_int_distribution<> dist(0, 10);
      constexpr auto msg = "Hello, Client!"sv;
      co_await yield(dist(gen));
      Fields fields;
      fields.set("Content-Length", std::to_string(msg.size()));
      co_await response.async_submit(200, fields);
      co_await yield(dist(gen));
      co_await response.async_write(asio::buffer(msg));
      co_await yield(dist(gen));
      co_await response.async_write({});
      co_await yield(dist(gen));
      std::array<uint8_t, 16> data;
      co_await request.async_read_some(asio::buffer(data));
   };
   test = [this](Session session) -> awaitable<void>
   {
      std::uniform_int_distribution<> dist(0, 10);
      for (size_t i = 0; i < 100; ++i)
      {
         std::println(
            "=== {} =========================================================================", i);
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
   };
}

TEST_P(ClientAsync, HelloWorld)
{
   static const auto hello = "Hello, World!"sv;
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      co_await response.async_write_eof(asio::buffer(hello));
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({});
      auto response = co_await request.async_get_response();
      auto body = co_await read(response);
      EXPECT_EQ(body, hello);
   };
}

// -------------------------------------------------------------------------------------------------

//
// A single async_write() larger than what the transport hands to its peer in one go, i.e. the
// whole body in one call instead of chunk by chunk. HTTP/3 has to keep offering the same buffer
// to nghttp3 across many packets here, and complete the write only once all of it is
// acknowledged.
//
TEST_P(ClientAsync, WHEN_server_writes_large_buffer_at_once_THEN_receives_all)
{
   static const std::vector<uint8_t> body = []
   {
      std::vector<uint8_t> data(256 * 1024);
      std::ranges::generate(data, [i = uint8_t(0)]() mutable { return i++; });
      return data;
   }();

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::array<uint8_t, 1024> buffer;
      while (co_await request.async_read_some(asio::buffer(buffer)) > 0)
         ; // drain the request -- HTTP/1.1 closes the connection on an unfinished parser

      co_await response.async_submit(200, fields({{"Content-Length", body.size()}}));
      co_await response.async_write(asio::buffer(body));
      co_await response.async_write({});
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({});
      EXPECT_EQ(co_await read_response(request), body.size());
   };
}

//
// Cancelling a response write mid-body. HTTP/3 hands the caller's buffer to nghttp3 by reference,
// so bytes already offered and not yet acknowledged cannot simply be abandoned -- the stream is
// reset instead, which is what the peer would see anyway for a body that stops short of its end.
//
TEST_P(ClientAsync, WHEN_server_cancels_write_THEN_client_sees_truncated_body)
{
   static const std::vector<uint8_t> body(8 * 1024 * 1024, 'x');

   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::array<uint8_t, 1024> buffer;
      while (co_await request.async_read_some(asio::buffer(buffer)) > 0)
         ; // drain the request -- HTTP/1.1 closes the connection on an unfinished parser

      co_await response.async_submit(200, {});

      //
      // Far more than the peer's receive window, and the client below doesn't read a byte until
      // this is over, so the write is guaranteed to still be in progress when it is cancelled.
      //
      auto executor = co_await this_coro::executor;
      auto [ep] = co_await co_spawn(executor, send(response, std::span(body)),
                                    cancel_after(50ms, as_tuple));
      EXPECT_EQ(code(ep), boost::system::errc::operation_canceled);
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({});
      auto response = co_await request.async_get_response();

      //
      // Leave the response body untouched for now: whatever the server manages to send fills up
      // the receive window and stays there, so its write cannot run to completion before the
      // cancellation above hits.
      //
      asio::steady_timer timer(co_await this_coro::executor, 150ms);
      co_await timer.async_wait(deferred);

      boost::system::error_code ec;
      auto received = co_await try_receive(response, ec);
      std::println("received {} of {} bytes ({})", received, body.size(), ec.message());
      EXPECT_LT(received, body.size());
      EXPECT_EQ(ec, boost::beast::http::error::partial_message);
   };
}

// =================================================================================================

//
// serve_file() mounted on "/custom", serving a directory tree created fresh for each testcase.
//
class FileHandler : public ClientAsync
{
public:
   void SetUp() override
   {
      base = std::filesystem::temp_directory_path() /
             std::format("anyhttp-file-handler-{}", ::getpid());
      root = base / "docroot";
      std::filesystem::remove_all(base);
      std::filesystem::create_directories(root / "sub");

      write(root / "hello.txt", "Hello, File!");
      write(root / "empty.txt", "");
      write(root / "sub" / "nested.txt", "Nested!");
      write(root / "large.bin", std::string(256 * 1024, 'x'));
      write(root / "secret.txt", "no peeking");
      write(root / "er.txt", "leaked"); // what "/customer.txt" resolves to without a segment check
      std::filesystem::permissions(root / "secret.txt", std::filesystem::perms::none);
      std::filesystem::create_symlink(base / "outside.txt", root / "escape.txt");

      // just outside the root, reachable only by escaping it -- so a missing check shows up as
      // content served instead of a 404
      write(base / "outside.txt", "outside");

      ClientAsync::SetUp();

      custom = [this](server::Request request, server::Response response) -> awaitable<void> {
         co_await serve_file(std::move(request), std::move(response), root, "/custom");
      };
   }

   void TearDown() override
   {
      ClientAsync::TearDown();
      std::filesystem::remove_all(base);
   }

   static void write(const std::filesystem::path& path, std::string_view content)
   {
      std::ofstream(path, std::ios::binary).write(content.data(), content.size());
   }

   //
   // Requests \p target and returns status code and body. The request is finished right away --
   // serve_file() ignores the request body, but still has to consume it.
   //
   awaitable<std::tuple<int, std::string>> get(Session& session, boost::urls::url target)
   {
      auto request = co_await session.async_submit(target, {});
      co_await request.async_write({});
      auto response = co_await request.async_get_response();
      auto body = co_await read(response);
      co_return std::make_tuple(response.status_code(), std::move(body));
   }

   awaitable<std::tuple<int, std::string>> get(Session& session, std::string_view path)
   {
      co_return co_await get(session, boost::urls::url(url).set_path(path));
   }

   /// Target with a percent-encoded path, passed to the server as-is.
   boost::urls::url encoded(std::string_view path) const
   {
      auto target = boost::urls::url(url);
      target.set_encoded_path(path);
      return target;
   }

protected:
   std::filesystem::path base; ///< holds the docroot and the file just outside of it
   std::filesystem::path root; ///< what serve_file() is mounted on
};

INSTANTIATE_TEST_SUITE_P(FileHandler, FileHandler,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                           anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(FileHandler, WHEN_file_exists_THEN_serves_content)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/hello.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "Hello, File!");
   };
}

TEST_P(FileHandler, WHEN_file_is_in_subdirectory_THEN_serves_content)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/sub/nested.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "Nested!");
   };
}

//
// An empty file cannot be mmap()ed at all, and must not send the empty buffer that already means
// EOF twice.
//
TEST_P(FileHandler, WHEN_file_is_empty_THEN_serves_empty_body)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/empty.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "");
   };
}

//
// Large enough that a single async_write() spans many QUIC packets, so the HTTP/3 write path has
// to keep handing out the caller's buffer across several write_pkt() rounds.
//
TEST_P(FileHandler, WHEN_file_is_large_THEN_serves_all_of_it)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/large.bin");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, std::string(256 * 1024, 'x'));
   };
}

TEST_P(FileHandler, WHEN_file_does_not_exist_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/missing.txt");
      EXPECT_EQ(status, 404);
      EXPECT_EQ(body, "");
   };
}

//
// A directory can be open()ed but not mapped, and we do not serve listings.
//
TEST_P(FileHandler, WHEN_path_is_a_directory_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/sub")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/")), 404);
   };
}

TEST_P(FileHandler, WHEN_path_escapes_the_root_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/../outside.txt")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/sub/../../outside.txt")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, encoded("/custom/%2e%2e/outside.txt"))), 404);
   };
}

//
// weakly_canonical() resolves the link, so a link out of the root is caught like any other escape.
//
TEST_P(FileHandler, WHEN_symlink_points_outside_the_root_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/escape.txt")), 404);
   };
}

//
// The mount prefix must match whole path segments, not just any leading characters: stripping
// "/custom" off "/customer.txt" would otherwise serve "er.txt" out of the docroot.
//
TEST_P(FileHandler, WHEN_prefix_matches_mid_segment_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/customer.txt");
      EXPECT_EQ(status, 404);
      EXPECT_EQ(body, "");
      EXPECT_EQ(std::get<0>(co_await get(session, "/customer/hello.txt")), 404);
   };
}

TEST_P(FileHandler, WHEN_file_is_not_readable_THEN_error_403)
{
   if (::geteuid() == 0)
      GTEST_SKIP() << "running as root, permissions do not apply";

   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/secret.txt");
      EXPECT_EQ(status, 403);
      EXPECT_EQ(body, "");
   };
}

TEST_P(FileHandler, WHEN_same_file_is_requested_twice_THEN_serves_it_twice)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<1>(co_await get(session, "/custom/hello.txt")), "Hello, File!");
      EXPECT_EQ(std::get<1>(co_await get(session, "/custom/hello.txt")), "Hello, File!");
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, ServerYieldFirst)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await yield(10);
      co_await response.async_submit(200, {});
      co_await yield(10);
      co_await response.async_write({});
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url);
      co_await request.async_write({});
      co_await read_response(request);
   };
}

// ----------------------------------------------------------------------------------------------

static std::optional<size_t> stackRemainingBytes()
{
   pthread_attr_t attr;
   if (pthread_getattr_np(pthread_self(), &attr) != 0)
      return std::nullopt;

   void* stack_base = nullptr;
   size_t stack_size = 0;
   if (pthread_attr_getstack(&attr, &stack_base, &stack_size) != 0)
   {
      pthread_attr_destroy(&attr);
      return std::nullopt;
   }

   pthread_attr_destroy(&attr);

   if (stack_base == nullptr || stack_size == 0)
      return std::nullopt;

   int local = 0;
   std::uintptr_t sp = reinterpret_cast<std::uintptr_t>(&local);
   std::uintptr_t base = reinterpret_cast<std::uintptr_t>(stack_base);

   return (sp >= base) ? std::optional(sp - base) : std::nullopt;
}

TEST_P(ClientAsync, Recursion)
{
#if __has_feature(address_sanitizer)
   GTEST_SKIP() << "skipped under address sanitizer";
#endif
   if (!stackRemainingBytes())
      GTEST_SKIP() << "unable to measure stack on this platform";

   test = [this](Session session) -> awaitable<void>
   {
      auto ex = co_await this_coro::executor;
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();

      // verify that immediate completion (here, due to an empty buffer) does not cause recursion
      std::array<uint8_t, 0> empty;
      co_await response.async_read_some(asio::buffer(empty));
      auto s0 = stackRemainingBytes().value();
      co_await response.async_read_some(asio::buffer(empty));
      auto s1 = stackRemainingBytes().value();
      EXPECT_EQ(s0, s1);

      // however, ASIO allows us to control this behavior using "immediate executors"
      co_await response.async_read_some(asio::buffer(empty), bind_immediate_executor(ex));
      auto s2 = stackRemainingBytes().value();
      EXPECT_GT(s1, s2);
   };
}

// ----------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Custom)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
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
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {});
      size_t bytes = 1024;
      auto res = co_await (send(request, bytes) && read_response(request));
      EXPECT_EQ(bytes, res);
   };
}

TEST_P(ClientAsync, IgnoreRequest)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await response.async_submit(200, {});
      co_await response.async_write({});
   };
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("content-length", "0");
      auto request = co_await session.async_submit(url, fields);
      auto res = co_await (send(request, 0) && read_response(request));
   };
}

TEST_P(ClientAsync, IgnoreRequestAndResponse)
{
   custom = [this](server::Request request, server::Response response) -> awaitable<void>
   {
      std::ignore = request;
      std::ignore = response;
      co_return;
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {});
      auto res = co_await (send(request, 0) && try_read_response(request));
      EXPECT_FALSE(res.has_value());
      std::println("ERROR: {}", res.error().message());
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, PostRange)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      // co_await request.async_write(asio::buffer("ping"sv)); // FIXME:
      auto response = co_await request.async_get_response();
      // std::string s(10ul * 1024 * 1024, 'a');
      // auto sender = send(request, std::string_view("blah"));
      // auto sender = send(request, std::string(10ul * 1024 * 1024, 'a'));
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
      auto received = co_await (std::move(sender) && count(response));
      loge("received: {}", received);
      EXPECT_EQ(received, 1 * 1024 * 1024);
   };
}

TEST_P(ClientAsync, PostRangeImmediate)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto sender = sendAndForceEOF(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
      auto received = co_await (std::move(sender) && read_response(request));
      loge("received: {}", received);
      EXPECT_EQ(received, 1 * 1024 * 1024);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
      constexpr size_t bytes = 1024;
      co_await send(request, bytes);
      EXPECT_EQ(co_await count(response), bytes);
   };
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
   test = [this](Session session) -> awaitable<void>
   {
      auto request1 = co_await session.async_submit(url.set_path("echo"), {});
      co_await request1.async_write(asio::buffer("Hello, Server #1!"sv));
      co_await request1.async_write({});

      auto request2 = co_await session.async_submit(url.set_path("echo"), {});
      co_await request2.async_write(asio::buffer("Hello, Server #2! XYZ"sv));
      co_await request2.async_write({});

      auto response1 = co_await request1.async_get_response();
      EXPECT_EQ(co_await count(response1), 17);

      auto response2 = co_await request2.async_get_response();
      EXPECT_EQ(co_await count(response2), 21);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, EatRequest)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("eat_request"), {});
      co_await send(request, 1024);
      auto response = co_await request.async_get_response();
      auto received = co_await count(response);
      EXPECT_EQ(received, 0);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Dump)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(
         url.set_path("dump space").set_params({{"blah", "white space"}, {"x", "y"}}), {});
      co_await send_eof(request);
      auto response = co_await request.async_get_response();
      auto dump = co_await read(response);
      EXPECT_THAT(dump, testing::HasSubstr("path: /dump space"));
      EXPECT_THAT(dump, testing::HasSubstr("  blah=white space"));
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(ClientAsync, Backpressure)
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
TEST_P(ClientAsync, CancellationContentLength)
{
   test = [this](Session session) -> awaitable<void>
   {
      const size_t length = 50ul * 1024 * 1024;
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
TEST_P(ClientAsync, Cancellation)
{
   test = [this](Session session) -> awaitable<void>
   {
      const size_t length = 50ul * 1024 * 1024;
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
TEST_P(ClientAsync, CancellationRange)
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

TEST_P(ClientAsync, PerOperationCancellation)
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

TEST_P(ClientAsync, CancelAfter)
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

      co_await request.async_write(asio::buffer("Hello, Client!"sv));
      co_await request.async_write({});
      auto received = co_await count(response);
   };
}

TEST_P(ClientAsync, WHEN_send_more_than_content_length_THEN_connection_is_reset)
{
   test = [this](Session session) -> awaitable<void>
   {
      Fields fields;
      fields.set("content-length", "1024");
      auto request = co_await session.async_submit(url.set_path("eat_request"), fields);
      auto response = co_await request.async_get_response();
      co_await count(response);

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

TEST_P(ClientAsync, ClientDropRequest)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {});
      auto response = co_await request.async_get_response();
   };
}

// =================================================================================================

TEST_P(ClientAsync, ResetServerDuringRequest)
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

TEST_P(ClientAsync, DISABLED_SpawnAndForget)
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

//
// A QUIC peer that goes away without a word -- a killed client, a machine that went to sleep in
// the middle of a request -- leaves the server nothing to react to: no CONNECTION_CLOSE arrives,
// and no further packet ever will. Only the idle timer can notice, and dropping the connection
// when it fires is what releases the session, its streams, and the request handlers suspended on
// them.
//
// Note that this is *not* what a client calling Session::reset() looks like: that one says
// goodbye, and the server cleans up right away by way of the draining period.
//
class Http3IdleTimeout : public testing::Test
{
protected:
   static constexpr auto IdleTimeout = 500ms;

   void SetUp() override
   {
      setupLogging();

      server.emplace(context.get_executor(), server::Config{.listen_address = "127.0.0.2",
                                                            .port = 0,
                                                            .idle_timeout = IdleTimeout});
      server->setRequestHandler(
         [this](server::Request request, server::Response response) -> awaitable<void>
      {
         co_await response.async_submit(200, {});

         //
         // Wait for a request body that never comes: this first read is where the handler is
         // suspended when the client freezes, and it must be resumed -- with an error -- once
         // the server gives up on the connection.
         //
         std::array<uint8_t, 1024> buffer;
         auto [ec, n] = co_await request.async_read_some(asio::buffer(buffer), as_tuple);
         handler_result.set_value(ec);
      });

      url.set_port_number(server->local_endpoint().port());
   }

   asio::io_context context;
   std::optional<server::Server> server;

   std::promise<boost::system::error_code> handler_result;
   boost::urls::url url{"http://127.0.0.2/echo"};
};

// -------------------------------------------------------------------------------------------------

TEST_F(Http3IdleTimeout, WHEN_client_vanishes_in_flight_THEN_idle_timer_drops_the_session)
{
   auto result = handler_result.get_future();

   //
   // The server has to keep running while the client is frozen, so it gets a thread of its own.
   //
   std::jthread server_thread([this] { run(context); });
   boost::scope::scope_exit stop_server([this] { context.stop(); });

   //
   // The client runs on its own io_context, which is what makes freezing it possible: stopping
   // that context takes the client off the air mid-request without unwinding anything, so no
   // CONNECTION_CLOSE is ever sent -- just like a client process that was killed.
   //
   asio::io_context client_context;
   client::Client client(client_context.get_executor(),
                         client::Config{.url = url, .protocol = anyhttp::Protocol::h3});

   //
   // Session, request and response are kept out here rather than in the coroutine frame, which is
   // destroyed as soon as the coroutine below returns: unwinding them would reset the stream, and
   // that is a packet -- the one thing this client must not send.
   //
   std::optional<Session> session;
   std::optional<client::Request> request;
   std::optional<client::Response> response;

   bool responded = false;
   co_spawn(client_context, [&]() -> awaitable<void>
   {
      session = co_await client.async_connect();
      request = co_await session->async_submit(url, {});
      response = co_await request->async_get_response();
      responded = true;
   }, detached);

   //
   // Run the client just far enough to have the request open and answered, then stop running it:
   // from here on it never touches its socket again.
   //
   while (client_context.run_one() && !responded);
   ASSERT_TRUE(responded) << "client never received a response";
   std::println("=== freezing the client, request still in flight ===");

   //
   // From here on the server is on its own. Without the idle timer dropping the connection, the
   // request handler stays suspended in async_read_some() forever, and the session it belongs to
   // sits in the server's connection table for good.
   //
   ASSERT_EQ(result.wait_for(5s), std::future_status::ready) << "request handler never completed";
   EXPECT_EQ(result.get(), boost::system::errc::connection_reset);

   //
   // Only now, on the way out, is the frozen client allowed to unwind: doing so earlier would
   // have sent the CONNECTION_CLOSE that this test is all about not sending.
   //
}

// =================================================================================================
