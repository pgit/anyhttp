#include "test_fixtures.hpp"

#include <string>

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
   /// Installs a request handler that drains the request and responds 200 with \p body.
   void respond_with(std::string body)
   {
      requestHandler = [body = std::move(body)](server::Request request,
                                                server::Response response) -> awaitable<void> {
         EXPECT_EQ(co_await drain(request), 0); // a GET has no body
         co_await response.async_submit(
            200, fields({{"Content-Length", body.size()}, {"X-Answer", 42}}));
         co_await response.async_write_eof(asio::buffer(body));
      };
   }
};

INSTANTIATE_TEST_SUITE_P(AsyncGet, AsyncGet,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(AsyncGet, WHEN_get_THEN_message_has_status_fields_and_body)
{
   respond_with("Hello, World!");
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url);
      EXPECT_EQ(message.result(), http::status::ok);
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message["x-answer"], "42");
      EXPECT_EQ(message.body(), "Hello, World!");
   };
}

TEST_P(AsyncGet, WHEN_response_has_no_body_THEN_body_is_empty)
{
   respond_with("");
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url);
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
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url.set_path("unknown"));
      EXPECT_EQ(message.result(), http::status::not_found);
   };
}

TEST_P(AsyncGet, WHEN_body_is_large_THEN_all_of_it_arrives)
{
   auto body = std::string(1_m, 'x');
   respond_with(body);
   clientSession = [this, body](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url);
      EXPECT_EQ(message.result_int(), 200);
      EXPECT_EQ(message.body().size(), body.size());
      EXPECT_EQ(message.body(), body);
   };
}

TEST_P(AsyncGet, WHEN_headers_are_given_THEN_they_arrive_with_the_request)
{
   requestHandler = [](server::Request request, server::Response response) -> awaitable<void> {
      EXPECT_EQ(request.fields()["x-question"], "what?");
      EXPECT_EQ(request.fields()["content-length"], "0");
      co_await drain(request);
      co_await response.async_submit(200, fields({{"Content-Length", 0}}));
      co_await response.async_write_eof();
   };
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url, fields({{"X-Question", "what?"}}));
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
   clientSession = [this](Session session) -> awaitable<void> {
      for (size_t i = 0; i < 2; ++i)
      {
         auto message = co_await session.async_get(url);
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
   requestHandler = [](server::Request request, server::Response response) -> awaitable<void> {
      co_await sleep(1s);
      std::ignore = co_await response.async_submit(200, {}, as_tuple);
      std::ignore = co_await response.async_write_eof(as_tuple);
   };
   clientSession = [this](Session session) -> awaitable<void> {
      auto [ec, message] = co_await session.async_get(url, {}, cancel_after(100ms, as_tuple));
      EXPECT_EQ(ec, boost::system::errc::operation_canceled);
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
   setupLogging();
   io_context context;
   tcp::acceptor acceptor(context, tcp::endpoint(ip::make_address("127.0.0.1"), 0));

   std::string head;
   co_spawn(
      context,
      [&]() -> awaitable<void> {
         auto socket = co_await acceptor.async_accept();
         co_await async_read_until(socket, dynamic_buffer(head), "\r\n\r\n");
         constexpr auto response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv;
         co_await async_write(socket, buffer(response));
         socket.shutdown(tcp::socket::shutdown_send);
      },
      detached);

   auto url = boost::urls::url("http://127.0.0.1");
   url.set_port_number(acceptor.local_endpoint().port());

   client::Client client(context.get_executor(),
                         {.url = url, .protocol = anyhttp::Protocol::http11});
   co_spawn(
      context,
      [&]() -> awaitable<void> {
         auto session = co_await client.async_connect();
         auto message = co_await session.async_get(url.set_path("/index.html"));
         EXPECT_EQ(message.result_int(), 200);
         EXPECT_EQ(message.body(), "hello");
      },
      [](const std::exception_ptr& ep) { EXPECT_FALSE(ep) << what(ep); });

   context.run();

   EXPECT_THAT(head, StartsWith("GET /index.html HTTP/1.1\r\n"));
   EXPECT_THAT(head, HasSubstr("Content-Length: 0\r\n"));
   EXPECT_THAT(head, Not(HasSubstr("chunked")));
}

// =================================================================================================
