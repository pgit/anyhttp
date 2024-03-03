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

awaitable<void> send(Request& request, size_t bytes)
{
   std::vector<uint8_t> buffer;
   buffer.resize(64 * 1024 - 1);
   co_await request.async_write(asio::buffer(buffer), deferred);
   co_await request.async_write({}, deferred);
}

awaitable<void> receive(Request& request, size_t bytes)
{
   auto response = co_await request.async_get_response(deferred);
   while (bytes > 0)
   {
      auto buf = co_await response.async_read_some(deferred);
      bytes -= buf.size();
   }
   co_await response.async_read_some(deferred);
}

awaitable<void> do_request(Session& session, boost::urls::url url)
{
   auto request = session.submit(url, {});

   size_t bytes = 64 * 1024 - 1;
   co_await (send(request, bytes) && receive(request, bytes));
}

awaitable<void> do_requests(any_io_executor executor, Session& session, boost::urls::url url)
{
   for (size_t i = 0; i < 1; ++i)
      co_await do_request(session, url);
}

awaitable<void> do_session(Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect(deferred);

   for (size_t i = 0; i < 1; ++i)
      co_spawn(client.executor(), do_requests(client.executor(), session, url), detached);
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
