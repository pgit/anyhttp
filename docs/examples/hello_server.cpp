//
// The first program of the programmer's guide: answers every request, over HTTP/1.1, HTTP/2 and
// HTTP/3, on one port. Takes the certificate chain and the key to use, and if there are none, the
// test PKI that the build makes, relative to the working directory.
//
#include <anyhttp/server.hpp>

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
using IoContext = boost::corosio::io_context;
#else
#include <boost/asio/io_context.hpp>
using IoContext = boost::asio::io_context;
#endif

#include <string_view>

using namespace anyhttp;

static Task<void> hello(server::Request request, server::Response response)
{
   constexpr std::string_view body = "Hello, World!\n";

   if (auto [ec] = co_await response.submit(200, fields({{"Content-Length", body.size()}})); ec)
      co_return;

   std::ignore = co_await response.write_eof(boost::asio::buffer(body));
}

int main(int argc, char* argv[])
{
   IoContext context;

   server::Config config{.port = 8080};
   if (argc == 3)
   {
      config.tls_certificate_chain = argv[1];
      config.tls_private_key = argv[2];
   }

   server::Server server(context.get_executor(), config);
   server.on_request(hello);

   context.run();
}
