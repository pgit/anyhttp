#include "anyhttp/client.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "range/v3/range/concepts.hpp"

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
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
   try
   {
      co_await timer.async_wait(deferred);
      logi("sleep: done");
   }
   catch (const boost::system::system_error& ec)
   {
      loge("sleep: {}", ec.what());
   }
}

// -------------------------------------------------------------------------------------------------

awaitable<void> echo(server::Request request, server::Response response)
{
   co_await response.async_submit(200, {}, deferred);
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
   co_await response.async_submit(404, {}, deferred);
   co_await response.async_write({}, deferred);
}

awaitable<void> eat_request(server::Request request, server::Response response)
{
   co_await response.async_submit(200, {}, deferred);
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
            else if (request.url().path() == "/detach")
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

TEST_F(External, nghttp2)
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

TEST_F(External, curl11)
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
      client::Config config{.url = url, .protocol = anyhttp::Protocol::http11};
      config.url.set_port_number(server->local_endpoint().port());
      client.emplace(context.get_executor(), config);
   }

protected:
   boost::urls::url url;
   std::optional<client::Client> client;
};

class ClientTest : public Client
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

// -------------------------------------------------------------------------------------------------

awaitable<void> send(client::Request& request, size_t bytes)
{
   std::vector<uint8_t> buffer;
   buffer.resize(bytes);
   try
   {
      co_await request.async_write(asio::buffer(buffer), deferred);
      logi("send: done, wrote {} bytes", bytes);
   }
   catch (const boost::system::system_error& ec)
   {
      loge("send: {}", ec.what());
   }
   co_await request.async_write({}, deferred);
}

awaitable<size_t> receive(client::Response& response)
{
   size_t bytes = 0;
   for (;;)
   {
      auto buf = co_await response.async_read_some(deferred);
      if (buf.empty())
         break;

      logd("receive: {}", buf.size());
      bytes += buf.size();
   }

   logi("receive: EOF after reading {} bytes", bytes);
   co_return bytes;
}

awaitable<size_t> get_response(client::Request& request)
{
   auto response = co_await request.async_get_response(asio::deferred);
   co_return co_await receive(response);
}

// -------------------------------------------------------------------------------------------------

TEST_F(Client, WHEN_post_data_THEN_receive_echo)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = co_await session.async_submit(url, {}, deferred);
         size_t bytes = 1024; //  * 1024 * 1024;
         auto res = co_await (send(request, bytes) && get_response(request));
         EXPECT_EQ(bytes, res);
      },
      completion_handler());
   context.run();
}

TEST_F(Client, WHEN_post_to_unknown_path_THEN_error_404)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = co_await session.async_submit(url.set_path("unknown"), {}, deferred);
         co_await send(request, 1024);
         auto response = co_await request.async_get_response(asio::deferred);
         auto received = co_await receive(response);
      },
      completion_handler());
   context.run();
}

TEST_F(Client, DISABLED_WHEN_server_discards_request_THEN_error_500)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = co_await session.async_submit(url.set_path("detach"), {}, deferred);
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
   requires /* ranges::sized_range<Range> && */ ranges::borrowed_range<Range> &&
            (!ranges::contiguous_range<Range>)
awaitable<void> send(client::Request& request, Range range, bool eof = true)
{
   size_t bytes = 0;
#if 1
   std::array<uint8_t, 1460> buffer;
#else
   std::vector<uint8_t> buffer;
   buffer.resize(1460);
#endif
   for (auto chunk : range | rv::chunk(buffer.size()))
   {
      auto end = std::ranges::copy(chunk, buffer.data()).out;
      bytes += end - buffer.data();
#if 0
      auto result = co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()),
                                                 as_tuple(deferred));
      if (auto ec = std::get<0>(result))
      {
         loge("send: (range) {}", ec.what());
         break;
      }
#else
      try
      {
         co_await request.async_write(asio::buffer(buffer.data(), end - buffer.data()), deferred);
      }
      catch (const boost::system::system_error& ec)
      {
         loge("send: (range) {}", ec.what());
         break;
      }
#endif
   }
   
   if (eof)
   {
      logi("send: finishing request");
      co_await this_coro::reset_cancellation_state();
      co_await request.async_write({}, deferred);
      logi("send: done afer writing {} bytes", bytes);
   }
}

awaitable<void> sendEOF(client::Request& request)
{
   logi("send: finishing request...");
   co_await request.async_write({}, deferred);
   logi("send: finishing request... done");
}

TEST_F(Client, PostRange)
{
   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         auto session = co_await client->async_connect(asio::deferred);
         auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
         co_await request.async_write(asio::buffer("ping"), deferred); // FIXME:
         auto response = co_await request.async_get_response(asio::deferred);
         // std::string s(10ul * 1024 * 1024, 'a');
         // auto sender = send(request, std::string_view("blah"));
         // auto sender = send(request, std::string(10ul * 1024 * 1024, 'a'));
         auto sender = send(request, rv::iota(uint8_t(0)) | rv::take(1 * 1024 * 1024));
         auto received = co_await (std::move(sender) && receive(response));
         loge("received: {}", received);
      },
      completion_handler());
   context.run();
}

// -------------------------------------------------------------------------------------------------

TEST_F(ClientTest, WHEN_request_is_sent_THEN_response_is_received_before_body_is_posted)
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

TEST_F(ClientTest, EatRequest)
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

TEST_F(ClientTest, Backpressure)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      auto sender = send(request, rv::iota(uint8_t(0)), /* eof */ false);
      co_await (std::move(sender) || sleep(1ms));
      auto received = co_await (sendEOF(request) && receive(response));
      fmt::println("transferred {} bytes", received);
   };
   context.run();
}

TEST_F(ClientTest, Cancellation)
{
   test = [&](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url.set_path("echo"), {}, deferred);
      auto response = co_await request.async_get_response(asio::deferred);
      auto sender = send(request, rv::iota(uint8_t(0))); //  | rv::take(1 * 1024 * 1024));
      auto received = co_await ((std::move(sender) || sleep(0ms)) && receive(response));
      fmt::println("transferred {} bytes", std::get<1>(received));
   };
   context.run();
}

// =================================================================================================
