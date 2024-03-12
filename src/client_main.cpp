#include "anyhttp/client.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

#include <fmt/ostream.h>

using namespace anyhttp;
using namespace anyhttp::client;
using namespace boost::asio;
using namespace std::chrono_literals;

using namespace boost::asio::experimental::awaitable_operators;

awaitable<void> sayHello(Request& request, std::string_view hello)
{
   logd("sayHello: saying hello...");
   co_await request.async_write(asio::buffer(hello), deferred);
   logd("sayHello: saying hello... done, sending EOF...");
   co_await request.async_write({}, deferred);
   logd("sayHello: saying hello... done, sending EOF... done");
}

awaitable<void> send(Request& request, size_t bytes)
{
   std::vector<uint8_t> buffer;
   buffer.resize(64 * 1024 - 1);
   co_await request.async_write(asio::buffer(buffer), deferred);
   co_await request.async_write({}, deferred);
}

awaitable<size_t> get_response(Request& request)
{
   logd("receive: waiting for response...");
   auto response = co_await request.async_get_response(deferred);
   logd("receive: waiting for response... done");
   size_t total = 0;
   for (;;)
   {
      logd("receive: async_read_some...");
      auto buf = co_await response.async_read_some(deferred);
      logd("receive: async_read_some... done, read {} bytes", buf.size());
      if (buf.empty())
         break;
      total += buf.size();
   }
   logd("receive: done, total {} bytes", total);
   co_return total;
}

awaitable<void> do_request(Session& session, boost::urls::url url)
{
   const std::string hello = "Hello, World!\r\n";
#if 0
   auto request = session.submit(url, {{"Content-Length", fmt::format("{}", hello.size())}});
#else
   auto request = session.submit(url, {{"X-Custom-Header", "Value"}});
#endif
#if 0
   size_t bytes = 64 * 1024 - 1;
   auto result = co_await (send(request, bytes) && receive(request));
   assert(bytes == result);
#else
   co_await (sayHello(request, hello) && get_response(request));
   logi("do_request: done");
#endif
}

awaitable<void> do_requests(any_io_executor executor, Session session, boost::urls::url url)
{
   for (size_t i = 0; i < 1; ++i)
      co_await do_request(session, url);
}

awaitable<void> do_session(Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect(deferred);

#if 1
   for (size_t i = 0; i < 1; ++i)
      co_await do_request(session, url);
#else
   for (size_t i = 0; i < 100; ++i)
      co_spawn(client.executor(), do_requests(client.executor(), session, url), detached);
#endif
}

int main(int argc, char* argv[])
{
   if (argc < 2)
   {
      fmt::println("Usage: {} URL", argv[0]);
      return 1;
   }

   io_context context;
   Config config{.url = boost::urls::url(argv[1]), .protocol = Protocol::http11};
   Client client(context.get_executor(), config);

   for (size_t i = 0; i < 1; ++i)
      co_spawn(context, do_session(client, config.url), detached);

   context.run();
   return 0;
}
