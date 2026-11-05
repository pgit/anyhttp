//
// The second program of the programmer's guide: one GET, over HTTP/2 unless told otherwise, which
// prints what comes back. Takes the URL, and for a server with a certificate of a private CA, the
// file that has that CA in it.
//
#include <anyhttp/client.hpp>
#include <anyhttp/session.hpp>

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
using IoContext = boost::corosio::io_context;
#else
#include <boost/asio/io_context.hpp>
using IoContext = boost::asio::io_context;
#endif

#include <print>
#include <string>

using namespace anyhttp;

static Task<void> fetch(client::Client& client, boost::urls::url url, int& result)
{
   auto [connect_ec, session] = co_await client.connect();
   if (connect_ec)
   {
      std::println(stderr, "connect: {}", connect_ec.message());
      co_return;
   }

   auto [get_ec, message] = co_await session.get(url);
   if (get_ec)
   {
      std::println(stderr, "get: {}", get_ec.message());
      co_return;
   }

   std::println("{} over {}: {}", message.result_int(), to_string(session.protocol()),
                message.body());
   result = 0;
}

int main(int argc, char* argv[])
{
   if (argc < 2 || argc > 3)
   {
      std::println(stderr, "usage: {} <url> [<CA file>]", argv[0]);
      return 2;
   }

   IoContext context;

   boost::urls::url url(argv[1]);
   client::Client client(context.get_executor(),
                         {.url = url, .tls_ca_file = argc == 3 ? argv[2] : ""});

   int result = 1;
   launch(context.get_executor(), fetch(client, url, result));
   context.run();
   return result;
}
