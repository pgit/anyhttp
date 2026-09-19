#include "test_fixtures.hpp"

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
   awaitable<Response> exchange(tcp::socket& socket, Request request)
   {
      request.set(http::field::host, std::format("127.0.0.2:{}", port()));
      request.prepare_payload();
      co_await http::async_write(socket, request);

      Response response;
      co_await http::async_read(socket, m_buffer, response);
      co_return response;
   }

   /// Reads what follows the last response, which must be the end of the stream and nothing else.
   awaitable<error_code> read_eof(tcp::socket& socket)
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
      co_spawn(context, std::move(task), [this](const std::exception_ptr& ep)
      {
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
   run([&]() -> awaitable<void>
   {
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
   run([&]() -> awaitable<void>
   {
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
   run([&]() -> awaitable<void>
   {
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
   run([&]() -> awaitable<void>
   {
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
