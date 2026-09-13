#include "test_fixtures.hpp"

#include <boost/algorithm/string/join.hpp>

#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <nghttp2/nghttp2ver.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <print>
#include <regex>

namespace bp = boost::process::v2;

// https://github.com/curl/curl/issues/10634 --> use custom built curl
#define CURL_PATH "/usr/local/bin/curl"
#define NGHTTP_PATH "/usr/local/bin/nghttp"
#define H2LOAD_PATH "/usr/local/bin/h2load"

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
   // clang-format on

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
      // clang-format on

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
   // clang-format on

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

//
// curl --http2 with an http:// URL asks for an upgrade to h2c. The first request is upgraded, the
// second one is sent as an HTTP/2 stream on the same connection.
//
TEST_F(ExternalCustom, curl_h2c_upgrade)
{
   auto url = std::format("http://127.0.0.2:{}/dump", server->local_endpoint().port());
   // clang-format off
   Args args = {"-sS", "-v", "--http2",
                "-w", "%{http_code} HTTP/%{http_version}\n",
                url + "?first", url + "?second"};
   // clang-format on
   auto future = spawn(CURL_PATH, std::move(args));
   run();

   const std::string output = future.get();
   EXPECT_THAT(output, testing::HasSubstr("query: first"));
   EXPECT_THAT(output, testing::HasSubstr("query: second"));

   std::string_view rest = output;
   size_t upgraded = 0;
   for (size_t pos; (pos = rest.find("200 HTTP/2\n")) != std::string_view::npos; ++upgraded)
      rest.remove_prefix(pos + 1);
   EXPECT_EQ(upgraded, 2) << output;
}

// =================================================================================================
