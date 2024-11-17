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


namespace ranges = std::ranges;
namespace rv = std::ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;

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

awaitable<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   co_await response.async_submit(200, {}, deferred);
   for (;;)
   {
      auto buffer = co_await request.async_read_some(deferred);
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
         else
            return not_found(std::move(request), std::move(response));
      });

   auto threads = rv::iota(0) | rv::take(0) |
                  rv::transform([&](int) { return std::thread([&] { context.run(); }); }) |
                  ranges::to<std::vector>();
   context.run();

   for (auto& thread : threads)
      thread.join();

   return 0;
}
