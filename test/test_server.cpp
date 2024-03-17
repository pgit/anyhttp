#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "range/v3/range/concepts.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/process.hpp>
#include <boost/process/args.hpp>

#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <fmt/ostream.h>

#include <range-v3/concepts/type_traits.hpp>
#include <range-v3/range/v3/view/chunk.hpp>
#include <range-v3/range/v3/view/iota.hpp>
#include <range-v3/range/v3/view/repeat_n.hpp>
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

awaitable<void> delayed(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

awaitable<void> detach(server::Request request, server::Response response)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_from_now(100ms);
   co_await eat_request(std::move(request), std::move(response));
}

// =================================================================================================

class Empty : public testing::Test
{
};

// -------------------------------------------------------------------------------------------------

//
// Server fixture with some default request handlers.
//
class Server : public testing::Test
{
protected:
   void SetUp() override
   {
      auto config = server::Config{.port = 0};
      server.emplace(context.get_executor(), config);
      server->setRequestHandlerCoro(
         [](server::Request request, server::Response response) -> awaitable<void>
         {
            if (request.url().path() == "/echo")
               return echo(std::move(request), std::move(response));
            else if (request.url().path() == "/eat_request")
               return eat_request(std::move(request), std::move(response));
            else if (request.url().path() == "/discard")
               return {};
            // return []() mutable -> awaitable<void> { co_return; }();
            else if (request.url().path() == "/detach")
               // return detach(std::move(request), std::move(response));
               co_spawn(request.executor(), detach(std::move(request), std::move(response)),
                        [&](const std::exception_ptr&)
                        { logi("client finished, resetting server"); });
            else
               return not_found(std::move(request), std::move(response));
            return []() mutable -> awaitable<void> { co_return; }();
         });
   }

   auto completion_handler()
   {
      return [this](const std::exception_ptr&)
      {
         logi("client finished, resetting server");
         server.reset();
      };
   }

protected:
   boost::asio::io_context context;
   std::optional<server::Server> server;
};

// -------------------------------------------------------------------------------------------------

class External : public Server
{
protected:
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

   std::filesystem::path testFile{"CMakeLists.txt"};
   size_t testFileSize = file_size(testFile);
};

// =================================================================================================

TEST_F(Server, StopBeforeStarted)
{
   server.reset();
   context.run();
}

TEST_F(Server, Stop)
{
   context.run_one();
   server.reset();
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(External, ngttp2)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/workspaces/nghttp2/install/bin/nghttp", {"-d", testFile, url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_F(External, curl2)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/bin/curl", {"--http2-prior-knowledge", "--data-binary",
                                         fmt::format("@{}", testFile), url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

TEST_F(External, cutl11)
{
   auto url = fmt::format("http://127.0.0.1:{}/echo", server->local_endpoint().port());
   auto future = spawn("/usr/bin/curl", {"--data-binary", fmt::format("@{}", testFile), url});
   context.run();
   EXPECT_EQ(future.get().size(), testFileSize);
}

// =================================================================================================

class Client : public Server
{
protected:
   void SetUp() override
   {
      Server::SetUp();
      url = boost::urls::url("http://127.0.0.1");
      url.set_port_number(server->local_endpoint().port());
      client::Config config{.url = url, .protocol = anyhttp::Protocol::http2};
      config.url.set_port_number(server->local_endpoint().port());
      client.emplace(context.get_executor(), config);
   }

protected:
   boost::urls::url url;
   std::optional<client::Client> client;
};

// -------------------------------------------------------------------------------------------------

awaitable<void> send(client::Request& request, size_t bytes)
{
   std::vector<uint8_t> buffer;
   buffer.resize(bytes);
   co_await request.async_write(asio::buffer(buffer), deferred);
   co_await request.async_write({}, deferred);
   logi("send: done");
}

awaitable<size_t> receive(client::Response& response)
{
   size_t bytes = 0;
   for (;;)
   {
      auto buf = co_await response.async_read_some(deferred);
      if (!buf.empty())
         logd("receive: {}", buf.size());
      else
      {
         logi("receive: EOF afeter reading {} bytes", bytes);
         break;
      }
      bytes += buf.size();
   }
   logi("receive: done");
   co_return bytes;
}

awaitable<size_t> get_response(client::Request& request)
{
   auto response = co_await request.async_get_response(asio::deferred);
   co_return co_await receive(response);
}

awaitable<void> do_request(client::Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect(asio::deferred);
   auto request = session.submit(url, {});
   size_t bytes = 1; //  * 1024 * 1024;
   auto res = co_await (send(request, bytes) && get_response(request));
   // assert(bytes == res);
}

// -------------------------------------------------------------------------------------------------

TEST_F(Client, WHEN_post_data_THEN_receive_echo)
{
   co_spawn(context, do_request(*client, url.set_path("/echo")), completion_handler());
   context.run();
}

TEST_F(Client, WHEN_post_to_unknown_path_THEN_error_404)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = session.submit(url.set_path("unknown"), {});
         co_await send(request, 1024);
         auto response = co_await request.async_get_response(asio::deferred);
         auto received = co_await receive(response);
      },
      completion_handler());
   context.run();
}

TEST_F(Client, WHEN_server_discards_request_THEN_error_500)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = session.submit(url.set_path("detach"), {});
         co_await send(request, 1024);
         auto response = co_await request.async_get_response(asio::deferred);
         auto received = co_await receive(response);
      },
      completion_handler());
   context.run();
}

// -------------------------------------------------------------------------------------------------

template <typename Range>
   requires ranges::contiguous_range<Range> && ranges::borrowed_range<Range>
awaitable<void> send(client::Request& request, Range range)
{
   co_await request.async_write(asio::buffer(range.data(), range.size()), deferred);
   co_await request.async_write({}, deferred);
   logi("send: done");
}

template <typename Range>
   requires ranges::sized_range<Range> && ranges::borrowed_range<Range> &&
            (!ranges::contiguous_range<Range>)
awaitable<void> send(client::Request& request, Range range)
{
   size_t bytes = 0;
   std::array<uint8_t, 16 * 1024> buffer;
   for (auto chunk : range | rv::chunk(buffer.size()))
   {
      auto end = std::ranges::copy(chunk, buffer.data()).out;
      bytes += end - buffer.data();
      co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()), deferred);
   }
   co_await request.async_write({}, deferred);
   logi("send: done afer writing {} bytes", bytes);
}

TEST_F(Client, PostRange)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = session.submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response(asio::deferred);
         // std::string s(10ul * 1024 * 1024, 'a');
         // auto sender = send(request, std::string_view(s));
         // auto sender = send(request, std::string(10ul * 1024 * 1024, 'a'));
         auto sender = send(request, rv::iota(uint8_t(0)) | rv::take(10 * 1024 * 1024));
         auto received = co_await (std::move(sender) && receive(response));
         loge("received: {}", received);
      },
      completion_handler());
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(Client, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = session.submit(url.set_path("echo"), {});
         auto response = co_await request.async_get_response(asio::deferred);
         size_t bytes = 1024;
         co_await send(request, bytes);
         auto received = co_await receive(response);
         assert(bytes == received);
      },
      completion_handler());
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(Client, EatRequest)
{
   co_spawn(context, do_request(*client, url.set_path("eat_request")),
            [&](const std::exception_ptr&) { server.reset(); });
   context.run();
}

// -------------------------------------------------------------------------------------------------

#if 0
TEST_F(Echo, DISLABED_Backpressure)
{
   client::Config config{.url = boost::urls::url("http://127.0.0.1/echo")};
   config.url.set_port_number(server->local_endpoint().port());
   client::Client client(context.get_executor(), config);
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
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
#endif

// =================================================================================================
