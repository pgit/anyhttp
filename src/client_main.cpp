#include "anyhttp/client.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <print>

using namespace anyhttp;
using namespace anyhttp::client;
using namespace boost::asio;
using namespace std::chrono_literals;

using namespace boost::asio::experimental::awaitable_operators;

awaitable<void> send(Request& request, std::string_view hello)
{
   logd("send: sending string of {} bytes...", hello.size());
   co_await request.async_write(asio::buffer(hello));
   logd("send: sending string of {} bytes... done, sending EOF...", hello.size());
   co_await request.async_write({});
   logd("send: sending string of {} bytes... done, sending EOF... done", hello.size());
}

awaitable<size_t> read_response(Request& request)
{
   logd("receive: waiting for response...");
   auto response = co_await request.async_get_response();
   logd("receive: waiting for response... done");
   size_t total = 0;
   std::array<uint8_t, 1024> buffer;
   for (;;)
   {
      logd("receive: async_read_some...");
      size_t n = co_await response.async_read_some(asio::buffer(buffer));
      logd("receive: async_read_some... done, read {} bytes", n);
      if (n == 0)
         break;
      total += n;
   }
   logd("receive: done, total {} bytes", total);
   co_return total;
}

awaitable<void> do_request(Session& session, boost::urls::url url)
{
   const std::string hello = "Hello, World!\r\n";
#if 0
   auto request = co_await session.async_submit(url, {{"Content-Length", std::format("{}", hello.size())}});
#else
   Fields fields;
   fields.set("X-Custom-Header", "value");
   auto request = co_await session.async_submit(url, fields);
#endif
#if 0
   size_t bytes = 64 * 1024 - 1;
   auto result = co_await (send(request, bytes) && receive(request));
   assert(bytes == result);
#else
   co_await (send(request, hello) && read_response(request));
   logi("do_request: done");
#endif
}

awaitable<void> do_requests(any_io_executor executor, Session session, boost::urls::url url)
{
   for (size_t i = 0; i < 833; ++i)
      co_await do_request(session, url);
}

awaitable<void> do_session(Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect();

#if 1
   for (size_t i = 0; i < 1; ++i)
      co_await do_request(session, url);   
#else
   for (size_t i = 0; i < 3; ++i)
      co_spawn(client.get_executor(), do_requests(client.get_executor(), session, url), detached);
#endif
   logi("do_requests: done");
}

int main(int argc, char* argv[])
{
   if (argc < 2)
   {
      std::println("Usage: {} URL", argv[0]);
      return 1;
   }

   io_context context;
   Config config{.url = boost::urls::url(argv[1]), .protocol = Protocol::h2};
   Client client(context.get_executor(), config);

   for (size_t i = 0; i < 1; ++i)
      co_spawn(context, do_session(client, config.url), detached);

   context.run();
   return 0;
}
