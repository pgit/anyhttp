#include "test_fixtures.hpp"

#include "anyhttp/h1_io.hpp"
#include "anyhttp/net.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#if !ANYHTTP_COROSIO
#include <boost/asio/ssl/host_name_verification.hpp>
#endif

#include <array>
#include <atomic>
#include <format>
#include <string>

#include <sys/socket.h>

using namespace testing;

namespace http = boost::beast::http;
namespace h1 = anyhttp::beast_impl::h1;

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

   Task<TcpSocket> connect()
   {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::connect(socket, {server->local_endpoint()}));
      co_return socket;
   }

   /// Writes \p request, with the Host field filled in, and reads the response that follows.
   template <typename Stream>
   Task<Response> exchange(Stream& socket, Request request)
   {
      request.set(http::field::host, std::format("127.0.0.2:{}", port()));
      request.prepare_payload();
      check(co_await h1::write_message(socket, request));

      Response response;
      check(co_await h1::read_message(socket, buffer_, response));
      co_return response;
   }

   /// Reads what follows the last response, which must be the end of the stream and nothing else.
   template <typename Stream>
   Task<error_code> read_eof(Stream& socket)
   {
      EXPECT_EQ(buffer_.size(), 0) << "unread data left over from the response";

      std::array<char, 64> buffer;
      auto [ec, n] = co_await io::read_some(socket, asio::buffer(buffer));
      if (!ec)
         ADD_FAILURE() << std::format("{} bytes after the last response: '{}'", n,
                                      std::string_view(buffer.data(), n));
      co_return ec;
   }

   /// Runs \p task to completion, then stops the server.
   void run(Task<void> task)
   {
      launch(context.get_executor(), std::move(task), [this](const std::exception_ptr& ep) {
         if (ep)
            ADD_FAILURE() << what(ep);
         server.reset();
      });
      Server::run();
   }

   boost::beast::flat_buffer buffer_;
};

// -------------------------------------------------------------------------------------------------

TEST_F(ConnectionClose, WHEN_request_asks_to_close_THEN_response_says_so_and_stream_ends)
{
   run([&]() -> Task<void> {
      auto socket = co_await connect();

      Request request{http::verb::get, "/dump", 11};
      request.set(http::field::connection, "close");
      auto response = co_await exchange(socket, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive()) << "the last response has to announce itself as one";
      EXPECT_EQ(co_await read_eof(socket), errors::eof);
   }());
}

TEST_F(ConnectionClose, WHEN_request_with_body_asks_to_close_THEN_body_is_served_first)
{
   run([&]() -> Task<void> {
      auto socket = co_await connect();

      Request request{http::verb::post, "/echo", 11};
      request.body() = "hello";
      request.set(http::field::connection, "close");
      auto response = co_await exchange(socket, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_EQ(response.body(), "hello");
      EXPECT_FALSE(response.keep_alive());
      EXPECT_EQ(co_await read_eof(socket), errors::eof);
   }());
}

TEST_F(ConnectionClose, WHEN_request_does_not_ask_to_close_THEN_connection_takes_the_next_request)
{
   run([&]() -> Task<void> {
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
   run([&]() -> Task<void> {
      auto socket = co_await connect();

      //
      // HTTP/1.0 has no persistent connections unless the client asks for one, so this response
      // is the last one even though nothing said "close".
      //
      auto response = co_await exchange(socket, Request{http::verb::get, "/dump", 10});

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive());
      EXPECT_EQ(co_await read_eof(socket), errors::eof);
   }());
}

// =================================================================================================

//
// A client that pipelines requests and then resets the connection leaves them in the server's
// buffer, where they can be parsed without touching the connection. Once the response to the first
// one has failed, the rest must be dropped: with ASIO, the third write on the dead socket would
// never complete, and the session would hang until the server is destroyed.
//
TEST_F(ConnectionClose, WHEN_peer_resets_with_requests_pipelined_THEN_the_rest_are_dropped)
{
   std::atomic<int> started = 0, finished = 0, failed = 0;
   requestHandler = [&](server::Request request, server::Response response) -> Task<void> {
      ++started;
      co_await sleep(200ms); // for the reset to arrive
      if (auto [ec] = co_await response.submit(200, fields({{"Content-Length", 0}})); ec)
         ++failed;
      ++finished;
   };

   //
   // Polls instead of waiting on an Event: the handler runs on a strand of its own under
   // MULTITHREADED.
   //
   auto until = [](const std::atomic<int>& counter, int value) -> Task<void> {
      for (int i = 0; counter < value && i < 500; ++i)
         co_await delay(10ms);
   };

   run([&]() -> Task<void> {
      auto socket = co_await connect();

      std::string requests;
      for (int i = 0; i < 3; ++i)
         requests += std::format("GET /custom HTTP/1.1\r\nHost: 127.0.0.2:{}\r\n\r\n", port());
      check(co_await io::write(socket, asio::buffer(requests)));

      //
      // Reset only once the server has the requests: data still in its receive queue may be lost
      // to the reset.
      //
      co_await until(started, 1);
      linger l{.l_onoff = 1, .l_linger = 0};
      EXPECT_EQ(::setsockopt(socket.native_handle(), SOL_SOCKET, SO_LINGER, &l, sizeof(l)), 0);
      io::close(socket);

      co_await until(finished, 1);
      co_await delay(300ms); // for the next request to be started, if it is going to be

      EXPECT_EQ(failed, 1) << "the response should have found the connection reset";
      EXPECT_EQ(started, 1) << "requests after a failed response must not be served";
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
   run([&]() -> Task<void> {
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
      auto send = [&]() -> Task<error_code> {
         auto [ec, n] = co_await io::write(socket, asio::buffer(request));
         co_return ec;
      };
      auto receive = [&]() -> Task<error_code> {
         auto [ec, n] = co_await h1::read_message(socket, buffer_, response);
         co_return ec;
      };

      auto [send_ec, receive_ec] = co_await when_both(send(), receive());
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
   /// TLS 1.3, with the certificate checked against the test PKI's root CA, for 127.0.0.2.
   Task<TlsStream> connect_tls()
   {
      auto stream = io::make_tls_stream(co_await connect(), tls_);
#if ANYHTTP_COROSIO
      stream.tls().set_hostname("127.0.0.2");
#else
      stream.set_verify_callback(asio::ssl::host_name_verification("127.0.0.2"));
#endif
      check(co_await io::handshake(stream, Role::client));
      co_return stream;
   }

#if ANYHTTP_COROSIO
   TlsContext tls_ = std::invoke([] {
      namespace corosio = boost::corosio;
      corosio::tls_context context;
      throw_on_error(context.set_min_protocol_version(corosio::tls_version::tls_1_3));
      throw_on_error(context.load_verify_file("pki/out/root.pem"));
      throw_on_error(context.set_verify_mode(corosio::tls_verify_mode::peer));
      return context;
   });
#else
   TlsContext tls_ = std::invoke([] {
      asio::ssl::context context{asio::ssl::context::tlsv13};
      context.load_verify_file("pki/out/root.pem");
      context.set_verify_mode(asio::ssl::verify_peer);
      return context;
   });
#endif

   static void throw_on_error(error_code ec)
   {
      if (ec)
         throw_error(ec);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(TlsConnectionClose, WHEN_request_asks_to_close_THEN_close_notify_comes_before_the_end)
{
   run([&]() -> Task<void> {
      auto stream = co_await connect_tls();

      Request request{http::verb::get, "/dump", 11};
      request.set(http::field::connection, "close");
      auto response = co_await exchange(stream, std::move(request));

      EXPECT_EQ(response.result_int(), 200);
      EXPECT_FALSE(response.keep_alive());

      // a clean end of stream, not ssl::error::stream_truncated
      EXPECT_EQ(co_await read_eof(stream), errors::eof);
   }());
}

// =================================================================================================
