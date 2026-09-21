#include "test_fixtures.hpp"

#include <boost/asio/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <format>
#include <string>

using namespace testing;

namespace http = boost::beast::http;

// =================================================================================================

//
// "Connection: close" over HTTP/1.1 (RFC 9112, section 9.6): a client that asks for the connection
// to end gets one last response, and that response has to say that it is the last one -- otherwise
// the client has no way of telling the end of the connection from one that was lost.
//
// The client side is a raw socket driven by hand, so that the test sees what actually goes over
// the wire, and whether the server ends the connection or drops it.
//
class ConnectionClose : public Server
{
protected:
   using Request = http::request<http::string_body>;
   using Response = http::response<http::string_body>;

   awaitable<tcp::socket> connect()
   {
      tcp::socket socket(co_await this_coro::executor);
      co_await socket.async_connect(server->local_endpoint());
      co_return socket;
   }

   /// Writes \p request, with the Host field filled in, and reads the response that follows.
   template <typename Stream>
   awaitable<Response> exchange(Stream& socket, Request request)
   {
      request.set(http::field::host, std::format("127.0.0.2:{}", port()));
      request.prepare_payload();
      co_await http::async_write(socket, request);

      Response response;
      co_await http::async_read(socket, m_buffer, response);
      co_return response;
   }

   /// Reads what follows the last response, which must be the end of the stream and nothing else.
   template <typename Stream>
   awaitable<error_code> read_eof(Stream& socket)
   {
      EXPECT_EQ(m_buffer.size(), 0) << "unread data left over from the response";

      std::array<char, 64> buffer;
      auto [ec, n] = co_await socket.async_read_some(asio::buffer(buffer), as_tuple);
      if (!ec)
         ADD_FAILURE() << std::format("{} bytes after the last response: '{}'", n,
                                      std::string_view(buffer.data(), n));
      co_return ec;
   }

   /// Runs \p task to completion, then stops the server.
   void run(awaitable<void> task)
   {
      co_spawn(context, std::move(task), [this](const std::exception_ptr& ep) {
         if (ep)
            ADD_FAILURE() << what(ep);
         server.reset();
      });
      Server::run();
   }

   boost::beast::flat_buffer m_buffer;
};

// -------------------------------------------------------------------------------------------------

TEST_F(ConnectionClose, WHEN_request_asks_to_close_THEN_response_says_so_and_stream_ends)
{
   run([&]() -> awaitable<void> {
      auto socket = co_await connect();

      Request request{http::verb::get, "/dump", 11};
      request.set(http::field::connection, "close");
      auto response = co_await exchange(socket, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive()) << "the last response has to announce itself as one";
      EXPECT_EQ(co_await read_eof(socket), asio::error::eof);
   }());
}

TEST_F(ConnectionClose, WHEN_request_with_body_asks_to_close_THEN_body_is_served_first)
{
   run([&]() -> awaitable<void> {
      auto socket = co_await connect();

      Request request{http::verb::post, "/echo", 11};
      request.body() = "hello";
      request.set(http::field::connection, "close");
      auto response = co_await exchange(socket, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_EQ(response.body(), "hello");
      EXPECT_FALSE(response.keep_alive());
      EXPECT_EQ(co_await read_eof(socket), asio::error::eof);
   }());
}

TEST_F(ConnectionClose, WHEN_request_does_not_ask_to_close_THEN_connection_takes_the_next_request)
{
   run([&]() -> awaitable<void> {
      auto socket = co_await connect();

      auto first = co_await exchange(socket, Request{http::verb::get, "/dump?first", 11});
      EXPECT_EQ(first.result_int(), 200);
      EXPECT_TRUE(first.keep_alive());
      EXPECT_THAT(first.body(), HasSubstr("query: first"));

      auto second = co_await exchange(socket, Request{http::verb::get, "/dump?second", 11});
      EXPECT_EQ(second.result_int(), 200);
      EXPECT_TRUE(second.keep_alive());
      EXPECT_THAT(second.body(), HasSubstr("query: second"));
   }());
}

TEST_F(ConnectionClose, WHEN_request_is_http_1_0_THEN_stream_ends_after_the_response)
{
   run([&]() -> awaitable<void> {
      auto socket = co_await connect();

      //
      // HTTP/1.0 has no persistent connections unless the client asks for one, so this response
      // is the last one even though nothing said "close".
      //
      auto response = co_await exchange(socket, Request{http::verb::get, "/dump", 10});

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive());
      EXPECT_EQ(co_await read_eof(socket), asio::error::eof);
   }());
}

// =================================================================================================

//
// A request whose header section is too large is answered with 431 and ends the connection, with
// the rest of the request still on its way. That is the one case here where the connection is
// closed while data is still coming in, and closing a socket with unread data in its receive
// queue is what makes the kernel send an RST instead of a FIN.
//
class RejectedRequest : public ConnectionClose
{
protected:
   static constexpr size_t limit = 4_k;

   void configure_server(server::Config& config) override { config.max_header_size = limit; }
};

TEST_F(RejectedRequest, WHEN_request_is_rejected_THEN_the_response_arrives_anyway)
{
   run([&]() -> awaitable<void> {
      auto socket = co_await connect();

      //
      // Far more than the server is willing to read: it stops at the limit, answers, and hangs
      // up, leaving the rest of this unread on the connection.
      //
      std::string request = std::format("GET /dump HTTP/1.1\r\nHost: 127.0.0.2:{}\r\n", port());
      for (size_t i = 0; request.size() < 4_m; ++i)
         request += std::format("x-header-{}: {}\r\n", i, std::string(1_k, 'a'));
      request += "\r\n";

      //
      // Sending and receiving have to overlap: the server stops reading long before the request
      // is out, so a write of all of it only completes once the connection is gone.
      //
      Response response;
      auto send = [&]() -> awaitable<error_code> {
         auto [ec, n] = co_await asio::async_write(socket, asio::buffer(request), as_tuple);
         co_return ec;
      };
      auto receive = [&]() -> awaitable<error_code> {
         auto [ec, n] = co_await http::async_read(socket, m_buffer, response, as_tuple);
         co_return ec;
      };

      auto [send_ec, receive_ec] = co_await (send() && receive());
      EXPECT_FALSE(receive_ec) << "the 431 was lost: " << receive_ec.message();
      EXPECT_EQ(response.result_int(), 431);
      EXPECT_FALSE(response.keep_alive());
   }());
}

// =================================================================================================

//
// The same over TLS, where ending the connection takes one more step: a "close_notify" that tells
// the peer that the end of the data is the end of the data, and not a connection that was cut.
// Without it, everything the peer reads afterwards fails with ssl::error::stream_truncated
// instead of a clean end of stream -- which is a truncation attack as far as TLS is concerned.
//
class TlsConnectionClose : public ConnectionClose
{
protected:
   using SslStream = asio::ssl::stream<tcp::socket>;

   awaitable<SslStream> connect_tls()
   {
      SslStream stream(co_await this_coro::executor, m_context);
      co_await stream.next_layer().async_connect(server->local_endpoint());
      co_await stream.async_handshake(asio::ssl::stream_base::client);
      co_return stream;
   }

   asio::ssl::context m_context = std::invoke([] {
      asio::ssl::context context{asio::ssl::context::tlsv13};
      context.load_verify_file("pki/out/root.pem");
      context.set_verify_mode(asio::ssl::verify_peer);
      context.set_verify_callback(asio::ssl::host_name_verification("127.0.0.2"));
      return context;
   });
};

// -------------------------------------------------------------------------------------------------

TEST_F(TlsConnectionClose, WHEN_request_asks_to_close_THEN_close_notify_comes_before_the_end)
{
   run([&]() -> awaitable<void> {
      auto stream = co_await connect_tls();

      Request request{http::verb::get, "/dump", 11};
      request.set(http::field::connection, "close");
      auto response = co_await exchange(stream, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive());

      // a clean end of stream, not ssl::error::stream_truncated
      EXPECT_EQ(co_await read_eof(stream), asio::error::eof);
   }());
}

// =================================================================================================
