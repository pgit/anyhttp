#include "test_fixtures.hpp"

#include <string>

using namespace testing;

// =================================================================================================

//
// Large and numerous header fields, in both directions.
//
// HTTP/2 splits a header block that does not fit into a single frame (16 KiB by default) into
// HEADERS + CONTINUATION frames, HTTP/3 compresses the whole field section with QPACK, and
// HTTP/1.1 parses it with a Beast parser. All of them are subject to Config::max_header_size.
//
class Headers : public ClientAsync
{
protected:
   void round_trip(Fields sent);
};

INSTANTIATE_TEST_SUITE_P(Headers, Headers,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

/// Generates \p count fields named x-header-<i>, each with a value of \p size characters.
static Fields make_fields(size_t count, size_t size)
{
   Fields result;
   for (size_t i = 0; i < count; ++i)
   {
      auto value = std::format("value-{}-", i);
      value.resize(std::max(size, value.size()), char('a' + i % 26));
      result.insert(std::format("x-header-{}", i), value);
   }
   return result;
}

/// All values of the fields named \p name, in the order they appear in.
static std::vector<std::string_view> values_of(const Fields& fields, std::string_view name)
{
   auto [begin, end] = fields.equal_range(name);
   return std::ranges::subrange(begin, end) |
          rv::transform([](auto& field) { return std::string_view(field.value()); }) |
          std::ranges::to<std::vector>();
}

/// Expects every field of \p expected to be found in \p actual.
static void expect_contains(const Fields& actual, const Fields& expected)
{
   for (auto&& field : expected)
      EXPECT_THAT(values_of(actual, field.name_string()), Contains(field.value()))
         << field.name_string();
}

/// Number of bytes the fields take up on an HTTP/1.1 wire, roughly.
static size_t wire_size(const Fields& fields)
{
   size_t result = 0;
   for (auto&& field : fields)
      result += field.name_string().size() + field.value().size() + 4; // ": " and CRLF
   return result;
}

// -------------------------------------------------------------------------------------------------

//
// Sends the fields with the request, and has the server send them back with the response.
//
void Headers::round_trip(Fields sent)
{
   custom = [sent](server::Request request, server::Response response) -> awaitable<void>
   {
      expect_contains(request.fields(), sent);
      co_await drain(request);
      auto fields = sent;
      fields.set("Content-Length", "0");
      co_await response.async_submit(200, fields);
      co_await response.async_write_eof();
   };
   test = [this, sent](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, sent);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 200);
      expect_contains(response.fields(), sent);
      EXPECT_EQ(co_await drain(response), 0);
   };
}

TEST_P(Headers, WHEN_many_small_headers_THEN_all_arrive_in_both_directions)
{
   round_trip(make_fields(200, 8)); // well below the 8 KiB HTTP/1.1 limit
}

TEST_P(Headers, WHEN_single_header_is_large_THEN_arrives_intact_in_both_directions)
{
   round_trip(make_fields(1, 6000));
}

TEST_P(Headers, WHEN_header_name_repeats_THEN_all_values_arrive_in_order)
{
   Fields sent;
   std::vector<std::string> values;
   for (size_t i = 0; i < 50; ++i)
      sent.insert("x-repeated", values.emplace_back(std::format("value-{}", i)));

   custom = [sent, values](server::Request request, server::Response response) -> awaitable<void>
   {
      EXPECT_THAT(values_of(request.fields(), "x-repeated"), ElementsAreArray(values));
      co_await drain(request);
      auto fields = sent;
      fields.set("Content-Length", "0");
      co_await response.async_submit(200, fields);
      co_await response.async_write_eof();
   };
   test = [this, sent, values](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, sent);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      EXPECT_THAT(values_of(response.fields(), "x-repeated"), ElementsAreArray(values));
      co_await drain(response);
   };
}

//
// Larger than a single HTTP/2 frame (16 KiB), so the header block goes out as HEADERS followed by
// CONTINUATION frames.
//
TEST_P(Headers, WHEN_headers_exceed_frame_size_THEN_all_arrive_in_both_directions)
{
   auto sent = make_fields(32, 1100);
   ASSERT_GT(wire_size(sent), 32_k);
   round_trip(sent);
}

TEST_P(Headers, WHEN_single_header_exceeds_frame_size_THEN_arrives_intact_in_both_directions)
{
   round_trip(make_fields(1, 40_k));
}

TEST_P(Headers, WHEN_request_headers_exceed_default_limit_THEN_server_responds_431)
{
   auto sent = make_fields(3, 30_k);
   ASSERT_GT(wire_size(sent), default_max_header_size);

   custom = [](server::Request request, server::Response response) -> awaitable<void>
   {
      ADD_FAILURE() << "request handler called for oversized request headers";
      co_await response.async_submit(200, {});
      co_await response.async_write_eof();
   };
   test = [this, sent](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, sent);
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 431);
   };
}

// =================================================================================================

//
// Header sections beyond Config::max_header_size, with a small limit on both sides.
//
// The receiving side stops storing fields as soon as the limit is exceeded. A server answers such a
// request with 431 without calling the request handler, a client fails async_get_response() with
// http::error::header_limit. Both HTTP/2 and HTTP/3 keep the session usable, as only the stream is
// affected, while an HTTP/1.1 connection is closed.
//
class HeaderLimits : public ClientAsync
{
protected:
   static constexpr size_t limit = 4_k;

   void configure_server(server::Config& config) override { config.max_header_size = limit; }
   void configure_client(client::Config& config) override { config.max_header_size = limit; }

   /// Request handler: responds with the headers of size \p response_size given as query parameter.
   void respond_with_headers()
   {
      custom = [this](server::Request request, server::Response response) -> awaitable<void>
      {
         ++handled;
         auto size = request.get_param_as<size_t>("response_size").value_or(0);
         co_await drain(request);
         auto fields = make_fields(1, size);
         fields.set("Content-Length", "0");
         auto [ec] = co_await response.async_submit(200, fields, as_tuple);
         if (!ec)
            std::tie(ec) = co_await response.async_write_eof(as_tuple);
         logi("server: {}", what(ec));
      };
   }

   /// Sends a request with \p sent headers, returns the response status code or error.
   awaitable<std::expected<unsigned int, error_code>> request(Session& session, const Fields& sent,
                                                              size_t response_size = 0)
   {
      auto target = url;
      if (response_size)
         target.params().set("response_size", std::to_string(response_size));

      auto [ec, request] = co_await session.async_submit(target, sent, as_tuple);
      if (!ec)
         std::tie(ec) = co_await request.async_write_eof(as_tuple);
      if (ec)
         co_return std::unexpected(ec);

      auto [ec2, response] = co_await request.async_get_response(as_tuple);
      if (ec2)
         co_return std::unexpected(ec2);
      co_await drain(response);
      co_return response.status_code();
   }

   size_t handled = 0;
};

INSTANTIATE_TEST_SUITE_P(HeaderLimits, HeaderLimits,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(HeaderLimits, WHEN_request_headers_are_within_limit_THEN_request_is_handled)
{
   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(co_await request(session, make_fields(1, limit / 2), limit / 2), 200);
      EXPECT_EQ(handled, 1);
   };
}

TEST_P(HeaderLimits, WHEN_request_headers_exceed_limit_THEN_server_responds_431)
{
   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(co_await request(session, make_fields(1, limit)), 431);
      EXPECT_EQ(handled, 0);
   };
}

//
// With HTTP/2 and HTTP/3, each field counts 32 bytes more than its name and value, so many small
// fields exceed the limit early.
//
TEST_P(HeaderLimits, WHEN_many_small_fields_exceed_limit_THEN_server_responds_431)
{
   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      auto sent = make_fields(200, 1);
      EXPECT_EQ(co_await request(session, sent), 431);
      EXPECT_EQ(handled, 0);
   };
}

//
// A header section 200 times the limit: the server must not store it, but tell the client.
//
// Except for HTTP/2, where it takes up more CONTINUATION frames than a header section within the
// limit ever needs. That is a flood, and nghttp2 closes the connection.
//
TEST_P(HeaderLimits, WHEN_request_headers_far_exceed_limit_THEN_request_is_rejected)
{
   auto sent = make_fields(25, 32_k);
   ASSERT_GT(wire_size(sent), limit * 200);

   respond_with_headers();
   test = [this, sent](Session session) -> awaitable<void>
   {
      auto result = co_await request(session, sent);
      if (GetParam() == anyhttp::Protocol::h2)
         EXPECT_FALSE(result.has_value()) << "status " << result.value_or(0);
      else
         EXPECT_EQ(result, 431);
      EXPECT_EQ(handled, 0);
   };
}

TEST_P(HeaderLimits, WHEN_request_is_rejected_THEN_session_serves_next_request)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP() << "HTTP/1.1 closes the connection after 431";

   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(co_await request(session, make_fields(1, limit)), 431);
      EXPECT_EQ(co_await request(session, make_fields(1, 100)), 200);
      EXPECT_EQ(handled, 1);
   };
}

// -------------------------------------------------------------------------------------------------

TEST_P(HeaderLimits, WHEN_response_headers_exceed_limit_THEN_get_response_fails)
{
   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      auto result = co_await request(session, {}, limit);
      EXPECT_EQ(result, std::unexpected(error_code(boost::beast::http::error::header_limit)));
   };
}

//
// The response has been rejected long before it is asked for: the failure must still be delivered.
//
TEST_P(HeaderLimits, WHEN_response_headers_exceed_limit_before_get_response_THEN_it_fails)
{
   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      auto target = url;
      target.params().set("response_size", std::to_string(limit));
      auto request = co_await session.async_submit(target, {});
      co_await request.async_write_eof();
      co_await sleep(100ms);
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::beast::http::error::header_limit) << what(ec);
   };
}

TEST_P(HeaderLimits, WHEN_response_is_rejected_THEN_session_serves_next_request)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP() << "HTTP/1.1 can not skip the rest of a response";

   respond_with_headers();
   test = [this](Session session) -> awaitable<void>
   {
      auto result = co_await request(session, {}, limit);
      EXPECT_EQ(result, std::unexpected(error_code(boost::beast::http::error::header_limit)));
      EXPECT_EQ(co_await request(session, {}, 100), 200);
      EXPECT_EQ(handled, 2);
   };
}
