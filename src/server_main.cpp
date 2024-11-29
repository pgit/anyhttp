#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fmt/ostream.h>

#include <ranges>

namespace rv = std::ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;

awaitable<void> hello_world(server::Request request, server::Response response)
{
   //
   // FIXME: h2spec adding this yield() breaks a lot of h2spec tests, even the generic ones
   //
   // co_await yield();
   auto buf = co_await request.async_read_some(deferred);
   co_await response.async_submit(200, {}, deferred);
   const char* literal = "Hello, World!\n";
   boost::asio::const_buffer buffer(literal, std::strlen(literal));
   co_await response.async_write(buffer, deferred);
   co_await response.async_write({}, deferred);
   while (!(co_await request.async_read_some(deferred)).empty())
      ;
}

void run(boost::asio::io_context& context)
{
#if 0
      context.run();
#else
   using namespace std::chrono;
   auto t0 = steady_clock::now();
   for (int i = 0; context.run_one(); ++i)
   {
      auto t1 = steady_clock::now();
      auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      if (dt < 100ms)
         std::println("--- {} "
                      "------------------------------------------------------------------------",
                      i);
      else
      {
         std::println("\x1b[1;31m--- {} ({}) "
                      "----------------------------------------------------------------"
                      "\x1b[0m",
                      i, dt);
      }
   }
#endif
}

int main()
{
   io_context context;
   auto executor = context.get_executor();
   auto server = std::make_optional<Server>(executor, Config{.port = 8080});

#if 1
   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](auto, auto)
      {
         fmt::println(" INTERRUPTED");
         logw("interrupt");
         server.reset();
      });
#endif

   server->setRequestHandlerCoro(
      [](server::Request request, server::Response response) -> awaitable<void>
      {
         if (request.url().path() == "/echo")
            return echo(std::move(request), std::move(response));
         else if (request.url().path() == "/")
            return hello_world(std::move(request), std::move(response));
         else
            return not_found(std::move(request), std::move(response));
      });

   auto threads = rv::iota(0) | rv::take(0) |
                  rv::transform([&](int) { return std::thread([&] { context.run(); }); }) |
                  ranges::to<std::vector>();

   run(context);

   for (auto& thread : threads)
      thread.join();

   return 0;
}
