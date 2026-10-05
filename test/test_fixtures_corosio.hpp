#pragma once

//
// The fixtures of test_fixtures.hpp for the COROSIO runtime, on a corosio::io_context.
// test_fixtures_asio.hpp has the same names and members, in the same order. MULTITHREADED (defined
// here, or on the compiler's command line) runs the tests on several threads, with a strand per
// connection.
//
#include <boost/capy/ex/run.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/corosio/io_context.hpp>

#include <stop_token>

/// The context the tests run their servers and clients on.
using IoContext = boost::corosio::io_context;

// =================================================================================================

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

// #define MULTITHREADED

//
// Server fixture with some default request handlers, see test_fixtures_asio.hpp.
//
class Server : public testing::TestWithParam<anyhttp::Protocol>
{
protected:
   //
   // Number of threads run() will run the io_context on. More than one makes the server put
   // every connection on its own strand.
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
      setup_logging();

      auto config = server::Config{.listen_address = "127.0.0.2", .port = 0};
      config.use_strand = threads() > 1;
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

   void run()
   {
      const size_t n = threads();
      if (n <= 1)
      {
         ::run(context);
         return;
      }

      auto pool =
         rv::iota(size_t{1}, n) |
         rv::transform([this](size_t) { return std::jthread([this] { context.run(); }); }) |
         std::ranges::to<std::vector>();

      context.run();
   }

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
#if defined(MULTITHREADED)
      client.emplace(new_strand(context.get_executor()), config);
#else
      client.emplace(context.get_executor(), config);
#endif
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
