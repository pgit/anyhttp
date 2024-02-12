#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/process.hpp>
#include <boost/process/args.hpp>

#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <fmt/ostream.h>

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

using namespace anyhttp;

// =================================================================================================

awaitable<void> sleep(auto duration)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(duration);
   co_await timer.async_wait(deferred);
}

// -------------------------------------------------------------------------------------------------

awaitable<void> echo(server::Request request, server::Response response)
{
   response.write_head(200, {});
   for (;;)
   {
      auto buffer = co_await request.async_read_some(deferred);
      if (buffer.empty())
         co_await sleep(100ms);
      co_await response.async_write(asio::buffer(buffer), deferred);
      if (buffer.empty())
         co_return;
   }
}

awaitable<void> not_found(server::Request request, server::Response response)
{
   response.write_head(404, {});
   co_await response.async_write({}, deferred);
}

awaitable<void> eat_request(server::Request request, server::Response response)
{
   response.write_head(200, {});
   co_await response.async_write({}, deferred);
   for (;;)
   {
      auto buffer = co_await request.async_read_some(deferred);
      if (buffer.empty())
         break;
   }
   co_await sleep(100ms);
}

// -------------------------------------------------------------------------------------------------

class Echo : public testing::Test
{
protected:
   void SetUp() override
   {
      using namespace server;
      auto config = Config{.port = 0};
      server.emplace(context.get_executor(), config);
      server->setRequestHandlerCoro(
         [](Request request, Response response) -> awaitable<void>
         {
            if (request.url().path() == "/echo")
               return echo(std::move(request), std::move(response));
            else if (request.url().path() == "/eat_request")
               return eat_request(std::move(request), std::move(response));
            else
               return not_found(std::move(request), std::move(response));
         });
   }

   awaitable<std::string> spawn_process(bp::filesystem::path path, std::vector<std::string> args)
   {
      bp::async_pipe out(context);
      bp::child child(
         path, std::move(args), bp::std_out > out, bp::std_err > bp::null,
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
            break;
      }

      child.wait(); // FIXME: this is sync
      logi("exit_code={}", child.exit_code());
      server.reset();
      co_return result;
   }

   auto spawn(bp::filesystem::path path, std::vector<std::string> args)
   {
      return co_spawn(context, spawn_process(std::move(path), std::move(args)), use_future);
   }

   boost::asio::io_context context;
   std::optional<anyhttp::server::Server> server;
   std::filesystem::path testFile{"CMakeLists.txt"};
   size_t testFileSize = file_size(testFile);
};

// =================================================================================================

TEST_F(Echo, NGHTTP)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/workspaces/nghttp2/install/bin/nghttp", {"-d", testFile, url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_F(Echo, Curl)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/bin/curl", {"--http2-prior-knowledge", "--data-binary",
                                         fmt::format("@{}", testFile), url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_F(Echo, CurlHttp11)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/bin/curl", {"--data-binary", fmt::format("@{}", testFile), url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

// =================================================================================================

awaitable<void> send(client::Request& request, size_t bytes)
{
   std::vector<uint8_t> buffer;
   buffer.resize(bytes);
   co_await request.async_write(asio::buffer(buffer), deferred);
   co_await request.async_write({}, deferred);
}

awaitable<size_t> receive(client::Request& request)
{
   auto response = co_await request.async_get_response(asio::deferred);
   size_t bytes = 0;
   for (;;)
   {
      auto buf = co_await response.async_read_some(deferred);
      if (buf.empty())
         break;
      bytes += buf.size();
   }
   co_return bytes;
}

awaitable<void> do_request(client::Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect(asio::deferred);
   auto request = session.submit(url, {});
   size_t bytes = 1 * 1024 * 1024;
   auto res = co_await (send(request, bytes) && receive(request));
   // assert(bytes == res);
}

// -------------------------------------------------------------------------------------------------

TEST_F(Echo, Client)
{
   client::Config config{.url = boost::urls::url("http://127.0.0.1/echo")};
   config.url.set_port_number(server->local_endpoint().port());
   client::Client client(context.get_executor(), config);
   co_spawn(context, do_request(client, config.url),
            [&](const std::exception_ptr&) { server.reset(); });
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(Echo, EatRequest)
{
   client::Config config{.url = boost::urls::url("http://127.0.0.1/eat_request")};
   config.url.set_port_number(server->local_endpoint().port());
   client::Client client(context.get_executor(), config);
   co_spawn(context, do_request(client, config.url),
            [&](const std::exception_ptr&) { server.reset(); });
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(Echo, Backpressure)
{
   client::Config config{.url = boost::urls::url("http://127.0.0.1/echo")};
   config.url.set_port_number(server->local_endpoint().port());
   client::Client client(context.get_executor(), config);
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client.async_connect(asio::deferred);
         auto request = session.submit(config.url, {});

         co_await send(request, 1024);
         co_await sleep(1s);
         auto response = co_await request.async_get_response(asio::deferred);
         for (;;)
         {
            auto buf = co_await response.async_read_some(deferred);
            if (buf.empty())
               break;
         }
      },
      [&](const std::exception_ptr&) { server.reset(); });
   context.run();
}

// =================================================================================================
