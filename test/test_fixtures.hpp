#pragma once

//
// Fixtures and helpers shared by the test_*.cpp files.
//
#include "anyhttp/client.hpp"
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

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <boost/scope/scope_exit.hpp>

#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <spdlog/common.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <ranges>
#include <thread>
#include <vector>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace asio = boost::asio;
using namespace asio;
using namespace asio::experimental::awaitable_operators;
using tcp = ip::tcp;

namespace rv = std::ranges::views;

using namespace anyhttp;

// =================================================================================================

/// Returns HTTP11 or HTTP/2 depending on the protocol.
static std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
}

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

         if (auto delay = request.get_param_as<std::chrono::milliseconds::rep>("delay"))
            co_await sleep(std::chrono::milliseconds{*delay});

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

// =================================================================================================
