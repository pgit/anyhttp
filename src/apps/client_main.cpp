//
// Sends one request, over HTTP/2 unless the server says otherwise, and counts the bytes of the
// response body. Takes the URL, the method, POST by default, which sends a short body, and for a
// server with a certificate of a private CA, the file that has that CA in it.
//
#include "anyhttp/client.hpp"
#include "anyhttp/request_handlers.hpp" // for count_response()
#include "anyhttp/session.hpp"

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
#else
#include <boost/asio/io_context.hpp>
#endif

#include <print>
#include <string>

using namespace anyhttp;

#if ANYHTTP_COROSIO
using IoContext = boost::corosio::io_context;
#else
using IoContext = boost::asio::io_context;
#endif

static Task<void> send(client::Request& request, std::string_view body)
{
   logd("send: sending string of {} bytes...", body.size());
   auto [ec, written] = co_await request.write_eof(boost::asio::buffer(body));
   if (ec)
      throw_error(ec);
   logd("send: sending string of {} bytes... done", body.size());
}

static Task<void> do_request(Session& session, boost::urls::url url, std::string_view method)
{
   const std::string body = method == "POST" ? "Hello, World!\r\n" : "";

   Fields fields;
   fields.set("X-Custom-Header", "value");
   auto [ec, request] = co_await session.submit(method, url, fields);
   if (ec)
      throw_error(ec);

   const size_t bytes = co_await when_both(send(request, body), count_response(request));
   logi("do_request: done, {} bytes of response body", bytes);
}

static Task<void> do_session(client::Client& client, boost::urls::url url, std::string_view method,
                             int& result)
{
   auto [ec, session] = co_await client.connect();
   if (ec)
   {
      std::println(stderr, "connect: {}", ec.message());
      co_return;
   }

   try
   {
      co_await do_request(session, url, method);
   }
   catch (const boost::system::system_error& ex)
   {
      std::println(stderr, "request: {}", ex.code().message());
      co_return;
   }

   if (auto [shutdown_ec] = co_await session.shutdown(); shutdown_ec)
      logw("shutdown: {}", shutdown_ec.message());

   logi("do_session: done");
   result = 0;
}

int main(int argc, char* argv[])
{
   if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
   {
      std::println("Usage: {} URL [METHOD [CA-FILE]]", argv[0]);
      return 0;
   }
   if (argc < 2 || argc > 4)
   {
      std::println(stderr, "Usage: {} URL [METHOD [CA-FILE]]", argv[0]);
      return 2;
   }

   const std::string_view method = argc > 2 ? argv[2] : "POST";

   IoContext context;
   client::Config config{.url = boost::urls::url(argv[1]),
                         .protocol = Protocol::h2,
                         .tls_ca_file = argc > 3 ? argv[3] : ""};
   client::Client client(context.get_executor(), config);

   int result = 1;
   launch(context.get_executor(), do_session(client, config.url, method, result));
   context.run();
   return result;
}
