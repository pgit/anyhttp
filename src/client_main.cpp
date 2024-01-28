#include "anyhttp/client.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fmt/ostream.h>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>

using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::client;

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

   context.run();
   return 0;
}
