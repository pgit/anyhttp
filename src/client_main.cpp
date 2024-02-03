#include "anyhttp/client.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
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

awaitable<void> do_request(Session& session, boost::urls::url url)
{
   auto request = session.submit(url, {});
   auto response = co_await request.async_get_response(asio::deferred);

   std::vector<uint8_t> buffer;
   buffer.resize(64 * 1024 - 1);

   for (size_t i = 0; i < 1; ++i)
   {
      co_await request.async_write(asio::buffer(buffer), deferred);
      co_await response.async_read_some(deferred);
   }

   co_await request.async_write({}, deferred);
   co_await response.async_read_some(deferred);
}

awaitable<void> do_requests(any_io_executor executor, Session& session, boost::urls::url url)
{
   for (size_t i = 0; i < 100000 / 10 / 4; ++i)
      co_await do_request(session, url);
}

awaitable<void> do_session(Client& client, boost::urls::url url)
{
   auto session = co_await client.async_connect(asio::deferred);

   for (size_t i = 0; i < 10; ++i)
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
   Config config{.url = boost::urls::url(argv[1])};
   Client client(context.get_executor(), config);

   for (size_t i = 0; i < 4; ++i)
      co_spawn(context, do_session(client, config.url), asio::detached);

   context.run();
   return 0;
}
