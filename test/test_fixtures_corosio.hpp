#pragma once

//
// The fixtures of test_fixtures.hpp for the COROSIO runtime: the same names and members, on a
// corosio::io_context, single-threaded. Shared tests use only what both have.
//
#include "anyhttp/client.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/capy/ex/run.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/corosio/io_context.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <vector>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace rv = std::ranges::views;

using namespace anyhttp;

/// The context the tests run their servers and clients on.
using IoContext = boost::corosio::io_context;

// =================================================================================================

/// Returns HTTP11, HTTP2 or HTTP3 depending on the protocol.
static std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
}

/// The protocols the parametrized shared tests run with.
inline std::vector<anyhttp::Protocol> protocols()
{
   return {anyhttp::Protocol::h1, anyhttp::Protocol::h2, anyhttp::Protocol::h3};
}

static void setup_logging()
{
#if defined(GITHUB_ACTIONS)
   spdlog::set_level(spdlog::level::warn);
#elif defined(NDEBUG)
   spdlog::set_level(spdlog::level::info);
#else
   spdlog::set_level(spdlog::level::debug);
#endif
}

/**
 * COROSIO's spelling of ASIO's cancel_after(): awaits \p task, which is requested to stop after
 * \p timeout. The request reaches only the task, not the coroutine that awaits it.
 */
template <typename T, typename Rep, typename Period>
Task<T> stop_after(std::chrono::duration<Rep, Period> timeout, Task<T> task)
{
   std::stop_source stop;
   Timer timer(co_await boost::capy::this_coro::executor);
   timer.arm(timeout, [&stop] { stop.request_stop(); });
   co_return co_await boost::capy::run(stop.get_token())(std::move(task));
}

// =================================================================================================

//
// Server fixture with some default request handlers, see test_fixtures.hpp.
//
class Server : public testing::TestWithParam<anyhttp::Protocol>
{
protected:
   void SetUp() override
   {
      setup_logging();

      auto config = server::Config{.listen_address = "127.0.0.2", .port = 0};
      configure_server(config);

      server.emplace(context.get_executor(), config);
      server->on_request([this](server::Request request, server::Response response) -> Task<void> {
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
            co_await requestHandler(std::move(request), std::move(response));
         else
            co_await not_found(std::move(request), std::move(response));
      });
   }

   void run() { ::run(context); }

   /// Lets a derived fixture adjust the server configuration before the server is created.
   virtual void configure_server(server::Config&) {}

   /// Returns listening port of the server.
   auto port() const noexcept { return server->local_endpoint().port(); }

protected:
   IoContext context;
   std::optional<server::Server> server;
   std::function<Task<void>(server::Request request, server::Response response)> requestHandler;
};

// =================================================================================================

class Client : public Server
{
protected:
   void SetUp() override
   {
      Server::SetUp();
      url.set_port_number(server->local_endpoint().port());
      client::Config config{.url = url, .protocol = GetParam(), .tls_ca_file = "pki/out/root.pem"};
      configure_client(config);
      client.emplace(context.get_executor(), config);
   }

   /// Lets a derived fixture adjust the client configuration before the client is created.
   virtual void configure_client(client::Config&) {}

protected:
   boost::urls::url url{"http://127.0.0.2/custom"};
   std::optional<client::Client> client;
};

// -------------------------------------------------------------------------------------------------

class ClientAsync : public Client
{
public:
   MOCK_METHOD(void, on_complete, (error_code ec), ());
   inline static const error_code Success{};

   void SetUp() override
   {
      Client::SetUp();

      //
      // Started from outside the running context, the test case does not begin before run().
      //
      auto test_case = [](ClientAsync* self) -> Task<void> {
         if (self->clientSession)
         {
            auto [ec, session] = co_await self->client->connect();
            if (ec)
               throw_error(ec);
            co_await self->clientSession(std::move(session));
         }
      };
      launch(client->get_executor(), test_case(this), [this](const std::exception_ptr& ep) {
         auto ec = code(ep);
         if (ec)
            logw("[{}] completed with \x1b[1;31m{}\x1b[0m", anyhttp::log_prefix(Role::client),
                 what(ec));
         else
            logi("[{}] completed successfully", anyhttp::log_prefix(Role::client));

         on_complete(ec);

         logd("[{}] stopping", anyhttp::log_prefix(Role::server));
         server.reset();
      });
   }

   void TearDown() override
   {
      EXPECT_CALL(*this, on_complete(error_code{}));
      run();
   }

public:
   std::function<Task<void>(Session session)> clientSession;
};

// =================================================================================================
