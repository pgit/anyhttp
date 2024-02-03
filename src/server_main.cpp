#include "anyhttp/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <csignal>
#include <iostream>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

namespace rv = ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;

int main()
{
   io_context pool;
   auto work = make_work_guard(pool);
   auto threads = rv::ints(0, 8) |
                  rv::transform([&](int) { return std::thread([&] { pool.run(); }); }) |
                  ranges::to<std::vector>;

   io_context context;
   auto server = std::make_optional<Server>(context.get_executor(), Config{.port = 8080});

#if 0
   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](auto, auto)
      {
         std::cout << std::endl << "INTERRUPTED" << std::endl;
         server.reset();
      });
#endif

   server->setRequestHandlerCoro(
      [&](Request request, Response response) -> awaitable<void>
      {
         response.write_head(200, {});
         try
         {
            for (;;)
            {
               logd("async_read_some...");
               auto buffer = co_await request.async_read_some(deferred);
               logd("async_read_some... done");
            // logi("{}", buffer.size());
#if 0
               auto timer = steady_timer(request.executor());
               timer.expires_after(1ms);
               logd("sleep...");
               co_await timer.async_wait(deferred);
               logd("sleep... done");
#endif
#if 0
               if (buffer.empty())
               {
                  // logi("hello from context ({})", std::this_thread::get_id());
                  co_await asio::post(bind_executor(pool, asio::deferred));
                  // std::this_thread::sleep_for(1ms); // simulate work in thread pool
                  // logi("hello from pool ({})", std::this_thread::get_id());
                  co_await asio::post(bind_executor(context, asio::deferred));
               }
#endif

               auto len = buffer.size();
               logd("async_write...");
               co_await response.async_write(asio::buffer(buffer), deferred);
               logd("async_write... done");

               if (len == 0)
                  break;
            }
         }
         catch (std::exception& ex)
         {
            logw("exception: {}", ex.what());
         }
         logd("done");
         co_return;
      });
   context.run();

   work.reset();
   for (auto& thread : threads)
      thread.join();

   return 0;
}
