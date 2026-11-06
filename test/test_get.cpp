#include "test_fixtures.hpp"

#include "anyhttp/literals.hpp"
#include "anyhttp/net.hpp"

#include "anyhttp/tls.hpp"

#include <spdlog/sinks/ringbuffer_sink.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace testing;

namespace http = boost::beast::http;

// =================================================================================================

//
// Session::async_get(): a whole GET request as a single operation, with the response handed back
// as a plain Beast message -- status, fields and the body as a string.
//
class AsyncGet : public ClientAsync
{
protected:
   void TearDown() override
   {
      ClientAsync::TearDown();
      // only now that every thread has stopped: the logger's sinks are a plain vector
      if (log_sink)
         std::erase(spdlog::default_logger_raw()->sinks(), log_sink);
   }

   /// Installs a request handler that drains the request and responds 200 with \p body.
   void respond_with(std::string body)
   {
      requestHandler = [body = std::move(body)](server::Request request,
                                                server::Response response) -> Task<void> {
         EXPECT_EQ(co_await drain(request), 0); // a GET has no body
         check(co_await response.submit(
            200, fields({{"Content-Length", body.size()}, {"X-Answer", 42}})));
         check(co_await response.write_eof(boost::asio::buffer(body)));
      };
   }

   /// A sink the test added to the default logger, which TearDown() takes off again.
   std::shared_ptr<spdlog::sinks::sink> log_sink;
};

INSTANTIATE_TEST_SUITE_P(AsyncGet, AsyncGet, ValuesIn(protocols()), NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(AsyncGet, WHEN_get_THEN_message_has_status_fields_and_body)
{
   respond_with("Hello, World!");
   clientSession = [this](Session session) -> Task<void> {
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.result(), http::status::ok);
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message["x-answer"], "42");
      EXPECT_EQ(message.body(), "Hello, World!");
   };
}

//
// The TLS handshake summary is only computed when logging at info, which CI builds do not. Turn it
// on for this one test so that tls_handshake_info() runs. setup_logging() in the next SetUp()
// restores the level.
//
TEST_P(AsyncGet, WHEN_logging_at_info_THEN_tls_handshake_is_summarized)
{
   auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256);
   log_sink = sink;
   spdlog::default_logger_raw()->sinks().push_back(sink);
   spdlog::set_level(spdlog::level::info);

   respond_with("Hello, World!");
   clientSession = [this, sink](Session session) -> Task<void> {
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.body(), "Hello, World!");

      auto handshake = HasSubstr("TLSv1.3, cipher=");
      if (GetParam() == anyhttp::Protocol::h3)
         EXPECT_THAT(sink->last_formatted(), Contains(handshake));
      else if (url.scheme() != "https") // for which corosio has less to say, see io::tls_info()
         EXPECT_THAT(sink->last_formatted(), Not(Contains(handshake)));
   };
}

TEST(TlsHandshakeInfo, WHEN_there_is_no_session_THEN_says_so)
{
   EXPECT_EQ(anyhttp::tls_handshake_info(nullptr), "no TLS session");
}

TEST_P(AsyncGet, WHEN_response_has_no_body_THEN_body_is_empty)
{
   respond_with("");
   clientSession = [this](Session session) -> Task<void> {
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_THAT(message.body(), IsEmpty());
   };
}

//
// A response is a response, whatever it says: only a request that gets none at all -- like the
// cancelled one below -- completes with an error.
//
TEST_P(AsyncGet, WHEN_path_is_unknown_THEN_message_says_404)
{
   clientSession = [this](Session session) -> Task<void> {
      auto message = check(co_await session.get(url.set_path("unknown")));
      EXPECT_EQ(message.result(), http::status::not_found);
   };
}

TEST_P(AsyncGet, WHEN_body_is_large_THEN_all_of_it_arrives)
{
   auto body = std::string(1_m, 'x');
   respond_with(body);
   clientSession = [this, body](Session session) -> Task<void> {
      auto message = check(co_await session.get(url));
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message.body().size(), body.size());
      EXPECT_EQ(message.body(), body);
   };
}

TEST_P(AsyncGet, WHEN_headers_are_given_THEN_they_arrive_with_the_request)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      EXPECT_EQ(request.fields()["x-question"], "what?");
      EXPECT_EQ(request.fields()["content-length"], "0");
      co_await drain(request);
      check(co_await response.submit(200, fields({{"Content-Length", 0}})));
      check(co_await response.write_eof());
   };
   clientSession = [this](Session session) -> Task<void> {
      auto message = check(co_await session.get(url, fields({{"X-Question", "what?"}})));
      EXPECT_EQ(message.result_int(), 200);
   };
}

//
// A GET is complete as soon as it has been submitted, so even HTTP/1.1, which allows just one
// request in progress at a time, takes the next one right away.
//
TEST_P(AsyncGet, WHEN_two_requests_in_a_row_THEN_both_are_answered)
{
   respond_with("Hello, World!");
   clientSession = [this](Session session) -> Task<void> {
      for (size_t i = 0; i < 2; ++i)
      {
         auto message = check(co_await session.get(url));
         EXPECT_EQ(message.result_int(), 200);
         EXPECT_EQ(message.body(), "Hello, World!");
      }
   };
}

//
// Cancellation reaches whichever of the steps the GET is waiting for, and comes out of the one
// operation the caller started. Nothing arrived, so the message stays empty.
//
TEST_P(AsyncGet, WHEN_cancelled_THEN_completes_with_operation_canceled_and_empty_message)
{
   //
   // Responds late, and to nobody in particular: by then the client has given up, so writing to
   // the stream is expected to fail.
   //
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      co_await sleep(1s);
      std::ignore = co_await response.submit(200, {});
      std::ignore = co_await response.write_eof();
   };
   clientSession = [this](Session session) -> Task<void> {
#if ANYHTTP_COROSIO
      auto [ec, message] = co_await stop_after(100ms, session.get(url));
#else
      auto [ec, message] = co_await session.async_get(url, {}, cancel_after(100ms, as_tuple));
#endif
      EXPECT_EQ(ec, errc::operation_canceled);
      EXPECT_EQ(message.result_int(), 0);
      EXPECT_THAT(message.body(), IsEmpty());
   };
}

// =================================================================================================

//
// What goes out on the wire, checked against a raw HTTP/1.1 peer: async_get() sends a GET, where
// async_submit() sends a POST, and frames the absent body with "Content-Length: 0" instead of
// making it chunked. HTTP/2 and HTTP/3 put the very same method string into ':method'.
//
TEST(AsyncGetRaw, WHEN_get_THEN_request_line_says_GET)
{
   setup_logging();
   IoContext context;
   auto acceptor = io::make_acceptor(context.get_executor());
   io::listen(acceptor, {io::make_address("127.0.0.1"), 0});

   std::string head;
   auto peer = [&]() -> Task<void> {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::accept(acceptor, socket));
      std::array<char, 1024> buffer;
      while (!head.contains("\r\n\r\n"))
      {
         auto [ec, n] = co_await io::read_some(socket, boost::asio::buffer(buffer));
         head.append(buffer.data(), n);
         if (ec)
            throw_error(ec);
      }
      constexpr auto response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv;
      check(co_await io::write(socket, boost::asio::buffer(response)));
      io::shutdown(socket, io::Shutdown::send);
   };
   launch(context.get_executor(), peer());

   auto url = boost::urls::url("http://127.0.0.1");
   url.set_port_number(io::local_endpoint(acceptor).port());

   client::Client client(context.get_executor(), {.url = url, .protocol = anyhttp::Protocol::h1});
   auto get = [&]() -> Task<void> {
      auto session = check(co_await client.connect());
      auto message = check(co_await session.get(url.set_path("/index.html")));
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message.body(), "hello");
   };
   launch(context.get_executor(), get(),
          [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

   context.run();

   EXPECT_THAT(head, StartsWith("GET /index.html HTTP/1.1\r\n"));
   EXPECT_THAT(head, HasSubstr("Content-Length: 0\r\n"));
   EXPECT_THAT(head, Not(HasSubstr("chunked")));
}

// =================================================================================================

//
// Session::submit() with a method: what is left to the caller compared to get(), which keeps every
// byte, is when to stop reading.
//
class SubmitMethod : public ClientAsync
{
protected:
   static Task<std::string> read_all(client::Response& response)
   {
      std::string body;
      std::array<char, 1_k> buffer;
      for (;;)
      {
         auto [ec, n] = co_await response.read_some(boost::asio::buffer(buffer));
         body.append(buffer.data(), n);
         if (ec == errors::eof)
            co_return body;
         if (ec)
            throw_error(ec);
      }
   }

   // members, as the test case runs in TearDown(), and they can be accessed from any thread
   std::atomic<size_t> sent = 0;
   std::atomic<bool> stopped = false;
};

INSTANTIATE_TEST_SUITE_P(SubmitMethod, SubmitMethod, ValuesIn(protocols()), NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(SubmitMethod, WHEN_method_is_given_THEN_the_server_sees_it)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      EXPECT_EQ(request.method(), "PUT");
      co_await echo(std::move(request), std::move(response));
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit("PUT", url));
      check(co_await request.write_eof(boost::asio::buffer("body"sv)));
      auto response = check(co_await request.get_response());
      EXPECT_EQ(response.status_code(), 200);
      EXPECT_EQ(co_await read_all(response), "body");
   };
}

//
// A body with no end, which the client stops reading at a limit of its choice. As the server can't
// finish, the test can only finish if the client doesn't wait for that.
//
TEST_P(SubmitMethod, WHEN_get_stops_reading_at_a_limit_THEN_status_and_fields_are_in_hand)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      EXPECT_EQ(request.method(), "GET");
      EXPECT_EQ(co_await drain(request), 0);
      check(co_await response.submit(200, fields({{"X-Answer", 42}})));

      std::vector<char> chunk(16_k);
      for (;;)
      {
         auto [ec] = co_await response.write(boost::asio::buffer(chunk));
         if (ec)
            break;
         sent += chunk.size();
      }
      stopped = true;
   };

   clientSession = [this](Session session) -> Task<void> {
      constexpr size_t limit = 256_k;

      auto request = check(co_await session.submit("GET", url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());
      EXPECT_EQ(response.status_code(), 200);
      EXPECT_EQ(response.fields()["x-answer"], "42");

      std::array<char, 16_k> buffer;
      size_t total = 0;
      while (total < limit)
         total += check(co_await response.read_some(boost::asio::buffer(buffer)));
      EXPECT_LE(total, limit + buffer.size());

      //
      // Not reading is what holds the server back: flow control, and with HTTP/1.1 the socket
      // buffers. Given the time, it would have sent gigabytes if nothing did.
      //
      co_await sleep(100ms);
      logi("sent {} bytes while the client read {}", sent.load(), total);
      EXPECT_LT(sent.load(), 64_m);

      //
      // Ending the connection is what stops the server, without anything else of the body being
      // read here.
      //
      response.reset();
      request.reset();
      session.reset();
      for (int i = 0; i < 2000 && !stopped; ++i)
         co_await sleep(1ms);
      EXPECT_TRUE(stopped);
   };
}

//
// HTTP/2 and HTTP/3 reset just the stream of a response released before its end, while HTTP/1.1,
// which has nothing but the connection to stop it with, can't send anything else.
//
TEST_P(SubmitMethod, WHEN_response_is_released_early_THEN_only_http11_loses_the_connection)
{
   requestHandler = [this](server::Request request, server::Response response) -> Task<void> {
      co_await drain(request);
      if (request.url().query() == "next")
      {
         check(co_await response.submit(200, fields({{"Content-Length", 0}})));
         check(co_await response.write_eof());
         co_return;
      }
      check(co_await response.submit(200, {}));
      std::vector<char> chunk(16_k);
      while (!std::get<0>(co_await response.write(boost::asio::buffer(chunk))))
         ;
      stopped = true;
   };

   clientSession = [this](Session session) -> Task<void> {
      {
         auto request = check(co_await session.submit("GET", url));
         check(co_await request.write_eof());
         auto response = check(co_await request.get_response());
         std::array<char, 1_k> buffer;
         check(co_await response.read_some(boost::asio::buffer(buffer)));
      }

      auto next = url;
      next.set_query("next");
      auto [ec, message] = co_await session.get(next);
      if (GetParam() == anyhttp::Protocol::h1)
         EXPECT_EQ(ec, errc::connection_aborted) << ec.message();
      else
      {
         EXPECT_EQ(ec, error_code{}) << ec.message();
         EXPECT_EQ(message.result_int(), 200);
         for (int i = 0; i < 2000 && !stopped; ++i)
            co_await sleep(1ms);
         EXPECT_TRUE(stopped) << "the server is still sending on the stream that was released";
      }
   };
}

//
// A response to HEAD says what the response to GET would have had, with nothing following it.
// HTTP/1.1 can't tell from the response that this is one, and would wait for the body.
//
TEST_P(SubmitMethod, WHEN_method_is_head_THEN_response_ends_after_its_header)
{
   requestHandler = [](server::Request request, server::Response response) -> Task<void> {
      EXPECT_EQ(request.method(), "HEAD");
      EXPECT_EQ(co_await drain(request), 0);
      check(co_await response.submit(200, fields({{"Content-Length", 5}})));
      check(co_await response.write_eof());
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = check(co_await session.submit("HEAD", url));
      check(co_await request.write_eof());
      auto response = check(co_await request.get_response());
      EXPECT_EQ(response.status_code(), 200);
      EXPECT_EQ(response.fields()["content-length"], "5");
      EXPECT_EQ(co_await read_all(response), "");
   };
}

// =================================================================================================
