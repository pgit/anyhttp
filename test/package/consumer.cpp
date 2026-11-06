//
// One GET over each protocol, from a client to a server in the same process, using nothing but
// the public headers. Takes the certificate and key of the server, which also serves as the CA.
//
#include <anyhttp/client.hpp>
#include <anyhttp/server.hpp>
#include <anyhttp/session.hpp>

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
using IoContext = boost::corosio::io_context;
#else
#include <boost/asio/io_context.hpp>
using IoContext = boost::asio::io_context;
#endif

#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>

using namespace anyhttp;

constexpr std::string_view body = "hello from anyhttp";

static Task<void> respond(server::Request request, server::Response response)
{
   std::array<uint8_t, 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await request.read_some(boost::asio::buffer(buffer));
      if (ec)
         break;
   }

   if (auto [ec] = co_await response.submit(200, {}); ec)
      co_return;
   co_await response.write_eof(boost::asio::buffer(body));
}

static Task<void> get(client::Client& client, std::optional<server::Server>& server,
                      boost::urls::url url, std::string& result)
{
   {
      auto [connect_ec, session] = co_await client.connect();
      if (connect_ec)
      {
         result = std::format("connect: {}", connect_ec.message());
         server.reset();
         co_return;
      }

      auto [get_ec, message] = co_await session.get(url);
      if (get_ec)
         result = std::format("get: {}", get_ec.message());
      else if (message.result_int() != 200 || message.body() != body)
         result = std::format("{}: '{}'", message.result_int(), message.body());
      else
         result = "ok";
   }
   server.reset();
}

int main(int argc, char* argv[])
{
   if (argc != 3)
   {
      std::println(stderr, "usage: {} <certificate> <key>", argv[0]);
      return 2;
   }

   int failed = 0;
   for (auto protocol : {Protocol::h1, Protocol::h2, Protocol::h3})
   {
      IoContext context;

      std::optional<server::Server> server;
      server.emplace(context.get_executor(), server::Config{.listen_address = "127.0.0.1",
                                                            .port = 0,
                                                            .tls_certificate_chain = argv[1],
                                                            .tls_private_key = argv[2]});
      server->on_request(respond);

      boost::urls::url url{"http://127.0.0.1/"};
      url.set_port_number(server->local_endpoint().port());
      client::Client client(
         context.get_executor(),
         client::Config{.url = url, .protocol = protocol, .tls_ca_file = argv[1]});

      std::string result = "did not finish";
      launch(context.get_executor(), get(client, server, url, result));
      context.run();

      std::println("{}: {}", to_string(protocol), result);
      if (result != "ok")
         ++failed;
   }
   return failed;
}
