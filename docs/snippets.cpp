//
// The code of the programmer's guide. The guide takes it from here, from one '[name]' comment to
// the next, and it is compiled along with everything else, so what the guide shows is what the
// library takes. test/test_docs.cpp runs it as well, so it also does what the guide says it does.
//
#include <anyhttp/client.hpp>
#include <anyhttp/literals.hpp>
#include <anyhttp/quic.hpp>
#include <anyhttp/server.hpp>
#include <anyhttp/session.hpp>

#if ANYHTTP_ASIO
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/io_context.hpp>
#else
#include <boost/corosio/io_context.hpp>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace snippets
{
using namespace anyhttp;
using namespace std::chrono_literals;

#if ANYHTTP_ASIO
using IoContext = boost::asio::io_context;
#else
using IoContext = boost::corosio::io_context;
#endif

// -------------------------------------------------------------------------------------------------
//
// Serving requests.
//

// [echo]
Task<void> serve_echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   if (auto [ec] = co_await response.submit(200, {}); ec)
      co_return;

   std::array<uint8_t, 64_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await request.read_some(boost::asio::buffer(buffer));
      if (ec == errors::eof)
         break;
      if (ec)
         co_return;

      if (auto [write_ec] = co_await response.write(boost::asio::buffer(buffer, n)); write_ec)
         co_return;
   }

   std::ignore = co_await response.write_eof();
}
// [echo-end]

// [write-at-once]
Task<void> respond_with(server::Response response, std::string body)
{
   if (auto [ec] = co_await response.submit(
          200, fields({{"Content-Length", body.size()}, {"Content-Type", "text/plain"}}));
       ec)
      co_return;

   std::ignore = co_await response.write_eof(boost::asio::buffer(body));
}
// [write-at-once-end]

// [route]
Task<void> route(server::Request request, server::Response response)
{
   const std::string path = request.url().path();
   if (path == "/echo")
      co_await serve_echo(std::move(request), std::move(response));
   else if (path == "/repeat")
   {
      const auto count = request.get_param_as<size_t>("count").value_or(1);
      co_await respond_with(std::move(response), std::string(count, '*'));
   }
   else
   {
      std::ignore = co_await response.submit(404, fields({{"Content-Length", 0}}));
      std::ignore = co_await response.write_eof();
   }
}
// [route-end]

// -------------------------------------------------------------------------------------------------
//
// Making requests.
//

// [read-all]
IoTask<std::string> read_all(Reader& reader)
{
   std::string body;
   std::array<char, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.read_some(boost::asio::buffer(buffer));
      body.append(buffer.data(), n);
      if (ec == errors::eof)
         co_return std::tuple{error_code{}, std::move(body)};
      if (ec)
         co_return std::tuple{ec, std::move(body)};
   }
}
// [read-all-end]

// [post]
IoTask<std::string> post(Session& session, boost::urls::url url, std::string body)
{
   auto [submit_ec, request] =
      co_await session.submit(url, fields({{"Content-Length", body.size()}}));
   if (submit_ec)
      co_return std::tuple{submit_ec, std::string{}};

   if (auto [ec] = co_await request.write_eof(boost::asio::buffer(body)); ec)
      co_return std::tuple{ec, std::string{}};

   auto [response_ec, response] = co_await request.get_response();
   if (response_ec)
      co_return std::tuple{response_ec, std::string{}};

   co_return co_await read_all(response);
}
// [post-end]

// [get]
Task<std::string> get_body(Session& session, boost::urls::url url)
{
   auto [ec, message] = co_await session.get(url);
   if (ec)
      throw_error(ec);

   std::println("{}: {} bytes", message.result_int(), message.body().size());
   co_return std::move(message.body());
}
// [get-end]

// [partial]
Task<size_t> read_at_most(Session session, boost::urls::url url, size_t limit)
{
   auto [submit_ec, request] = co_await session.submit("GET", url);
   if (submit_ec)
      co_return 0;
   std::ignore = co_await request.write_eof();

   auto [response_ec, response] = co_await request.get_response();
   if (response_ec)
      co_return 0;
   std::println("{} {}", response.status_code(),
                std::string_view(response.fields()["content-type"]));

   size_t total = 0;
   std::array<char, 16_k> buffer;
   while (total < limit)
   {
      auto [ec, n] = co_await response.read_some(boost::asio::buffer(buffer));
      total += n;
      if (ec)
         break;
   }

   session.reset(); // the rest of the body is never read
   co_return total;
}
// [partial-end]

// [both-ways]
Task<void> upload(Writer& writer, std::string_view body)
{
   for (; !body.empty(); body.remove_prefix(std::min(body.size(), size_t{16_k})))
      if (auto [ec] = co_await writer.write(boost::asio::buffer(body.substr(0, 16_k))); ec)
         throw_error(ec);

   if (auto [ec] = co_await writer.write_eof(); ec)
      throw_error(ec);
}

Task<std::string> download(client::Request& request)
{
   auto [response_ec, response] = co_await request.get_response();
   if (response_ec)
      throw_error(response_ec);

   auto [ec, body] = co_await read_all(response);
   if (ec)
      throw_error(ec);
   co_return std::move(body);
}

Task<std::string> round_trip(Session& session, boost::urls::url url, std::string_view body)
{
   auto [ec, request] = co_await session.submit(url);
   if (ec)
      throw_error(ec);

   co_return co_await when_both(upload(request, body), download(request));
}
// [both-ways-end]

// [would-block]
IoTask<client::Request> submit_patiently(Session& session, boost::urls::url url)
{
   for (;;)
   {
      auto [ec, request] = co_await session.submit(url);
      if (ec != errors::would_block)
         co_return std::tuple{ec, std::move(request)};

      std::ignore = co_await delay(10ms);
   }
}
// [would-block-end]

// [timeout]
Task<std::optional<client::Message>> get_or_give_up(Session& session, boost::urls::url url,
                                                    std::chrono::milliseconds limit)
{
   auto result = co_await when_either(session.get(url), delay(limit));
   if (result.index() == 1)
      co_return std::nullopt; // out of time, and the request has been cancelled

   auto [ec, message] = std::get<0>(std::move(result));
   if (ec)
      co_return std::nullopt;
   co_return std::move(message);
}
// [timeout-end]

// [shutdown]
Task<void> say_goodbye(Session session)
{
   if (auto [ec] = co_await session.shutdown(); ec)
      std::println("the connection could not be ended in an orderly way: {}", ec.message());
}
// [shutdown-end]

// -------------------------------------------------------------------------------------------------
//
// What a client connects to. These are kept short, and all they do is make the client.
//

inline client::Client tls_client(Executor executor)
{
   // [tls]
   client::Client client(executor, {.url = boost::urls::url("https://example.org")});
   // [tls-end]
   return client;
}

inline client::Client private_ca_client(Executor executor)
{
   // [private-ca]
   client::Client client(executor, {.url = boost::urls::url("https://localhost:8080"),
                                    .tls_ca_file = "pki/out/root.pem"});
   // [private-ca-end]
   return client;
}

inline client::Client h1_client(Executor executor)
{
   // [cleartext]
   client::Client client(
      executor, {.url = boost::urls::url("http://localhost:8080"), .protocol = Protocol::h1});
   // [cleartext-end]
   return client;
}

inline client::Client endpoints_client(Executor executor, IpAddress address)
{
   // [endpoints]
   client::Client client(executor, {.url = boost::urls::url("https://example.org"),
                                    .endpoints = {TcpEndpoint(address, 8443)}});
   // [endpoints-end]
   return client;
}

// -------------------------------------------------------------------------------------------------
//
// Running it.
//

// [stop]
Task<void> serve_for(std::optional<server::Server>& server, std::chrono::milliseconds duration)
{
   std::ignore = co_await delay(duration);
   server.reset(); // on the server's own executor, which this coroutine has been started on
}

inline void run_for_a_while(server::Config config)
{
   IoContext context;

   std::optional<server::Server> server;
   server.emplace(context.get_executor(), config);
   server->on_request(route);

   launch(context.get_executor(), serve_for(server, 100ms));
   context.run(); // returns when all of it has come to an end
}
// [stop-end]

// [threads]
inline void run_on_threads(server::Config config, size_t count)
{
   IoContext context(static_cast<int>(count));

   config.use_strand = true;
   std::optional<server::Server> server;
   server.emplace(context.get_executor(), config);
   server->on_request(route);

   launch(context.get_executor(), serve_for(server, 100ms));

   std::vector<std::thread> threads;
   for (size_t i = 1; i < count; ++i)
      threads.emplace_back([&context] { context.run(); });
   context.run();
   for (auto& thread : threads)
      thread.join();
}
// [threads-end]

// -------------------------------------------------------------------------------------------------
//
// Plain QUIC streams.
//

// [quic-echo]
Task<void> quic_echo(quic::Stream stream)
{
   std::array<uint8_t, 16_k> buffer;
   for (;;)
   {
      auto [ec, n] = co_await stream.read_some(boost::asio::buffer(buffer));
      if (ec == errors::eof)
         break;
      if (ec)
         co_return;

      if (auto [write_ec] = co_await stream.write(boost::asio::buffer(buffer, n)); write_ec)
         co_return;
   }
   std::ignore = co_await stream.write_eof();
}
// [quic-echo-end]

inline void quic_serve(server::Server& server)
{
   // [quic-server]
   server.on_quic({.alpn = "echo"}, [](quic::Connection connection) -> Task<void> {
      for (;;)
      {
         auto [ec, stream] = co_await connection.accept_stream();
         if (ec)
            co_return; // the connection is over
         launch(connection.get_executor(), quic_echo(std::move(stream)));
      }
   });
   // [quic-server-end]
}

// [quic-client]
IoTask<std::string> quic_hello(Executor executor, std::string host, std::string port,
                               std::string ca_file)
{
   auto [connect_ec, connection] = co_await quic::connect(
      executor, std::move(host), std::move(port), {.alpn = "echo", .tls_ca_file = ca_file});
   if (connect_ec)
      co_return std::tuple{connect_ec, std::string{}};

   auto [open_ec, stream] = connection.open_stream();
   if (open_ec)
      co_return std::tuple{open_ec, std::string{}};

   if (auto [ec] = co_await stream.write_eof(boost::asio::buffer("hello", 5)); ec)
      co_return std::tuple{ec, std::string{}};

   co_return co_await read_all(stream);
}
// [quic-client-end]

// -------------------------------------------------------------------------------------------------
//
// The shape of an operation on either runtime, with the types the aliases stand for.
//

#if ANYHTTP_ASIO
// [shape-asio]
boost::asio::awaitable<std::size_t> read_twice(Reader& reader, boost::asio::mutable_buffer buffer)
{
   // The asynchronous operation, with a completion token that reports errors instead of throwing.
   auto [ec, n] = co_await reader.async_read_some(buffer, boost::asio::as_tuple);
   if (ec)
      co_return n;

   // The coroutine around it.
   boost::asio::awaitable<std::tuple<boost::system::error_code, std::size_t>> next =
      reader.read_some(buffer + n);
   auto [next_ec, m] = co_await std::move(next);
   co_return n + m;
}
// [shape-asio-end]
#else
// [shape-corosio]
boost::capy::task<std::size_t> read_twice(Reader& reader, boost::asio::mutable_buffer buffer)
{
   // The coroutine, which is all there is.
   auto [ec, n] = co_await reader.read_some(buffer);
   if (ec)
      co_return n;

   boost::capy::task<std::tuple<std::error_code, std::size_t>> next = reader.read_some(buffer + n);
   auto [next_ec, m] = co_await std::move(next);
   co_return n + m;
}
// [shape-corosio-end]
#endif

// -------------------------------------------------------------------------------------------------
//
// What only the ASIO runtime has.
//

#if ANYHTTP_ASIO

// [token-exception]
Task<std::string> token_exception(Session& session, boost::urls::url url)
{
   auto message = co_await session.async_get(url);
   co_return std::move(message.body());
}
// [token-exception-end]

// [token-tuple]
Task<std::string> token_tuple(Session& session, boost::urls::url url)
{
   auto [ec, message] = co_await session.async_get(url, {}, boost::asio::as_tuple);
   if (ec)
      co_return ec.message();
   co_return std::move(message.body());
}
// [token-tuple-end]

// [token-callback]
void token_callback(Session& session, boost::urls::url url)
{
   session.async_get(url, {}, [](error_code ec, client::Message message) {
      if (!ec)
         std::println("{}", message.body());
   });
}
// [token-callback-end]

// [cancel-after]
Task<bool> response_within(client::Request& request, std::chrono::milliseconds limit)
{
   auto [ec, response] =
      co_await request.async_get_response(boost::asio::cancel_after(limit, boost::asio::as_tuple));
   co_return ec != errc::operation_canceled;
}
// [cancel-after-end]

#endif

} // namespace snippets
