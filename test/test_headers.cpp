#include "test_fixtures.hpp"

#include <string>

using namespace testing;

// =================================================================================================

//
// Large and numerous header fields, in both directions.
//
// HTTP/2 splits a header block that does not fit into a single frame (16 KiB by default) into
// HEADERS + CONTINUATION frames, HTTP/3 compresses the whole field section with QPACK, and
// HTTP/1.1 parses it with a Beast parser that has a header limit of its own.
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
// CONTINUATION frames. This is beyond the 8 KiB header limit of the HTTP/1.1 parser, see below.
//
TEST_P(Headers, WHEN_headers_exceed_frame_size_THEN_all_arrive_in_both_directions)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP() << "exceeds the HTTP/1.1 header limit";

   auto sent = make_fields(32, 1100);
   ASSERT_GT(wire_size(sent), 32_k);
   round_trip(sent);
}

TEST_P(Headers, WHEN_single_header_exceeds_frame_size_THEN_arrives_intact_in_both_directions)
{
   if (GetParam() == anyhttp::Protocol::http11)
      GTEST_SKIP() << "exceeds the HTTP/1.1 header limit";

   round_trip(make_fields(1, 40_k));
}

// -------------------------------------------------------------------------------------------------

//
// Headers beyond what the receiving side accepts: the request fails, but neither hangs nor crashes,
// and the request handler never sees it.
//
// For HTTP/1.1, the limit is the 8 KiB of the Beast parser. nghttp2 refuses to send a header block
// larger than 64 KiB. The fields are spread over many values, as Beast limits a single field to
// 64 KiB already.
//
// HTTP/3 has no limit: nghttp3 advertises an unlimited SETTINGS_MAX_FIELD_SECTION_SIZE by default.
//
TEST_P(Headers, WHEN_request_headers_exceed_limit_THEN_request_fails)
{
   if (GetParam() == anyhttp::Protocol::h3)
      GTEST_SKIP() << "no field section size limit for HTTP/3";

   auto sent = GetParam() == anyhttp::Protocol::http11 ? make_fields(1, 10_k) //
                                                       : make_fields(32, 32_k);
   custom = [](server::Request request, server::Response response) -> awaitable<void>
   {
      ADD_FAILURE() << "request handler called for oversized request headers";
      co_await response.async_submit(200, {});
      co_await response.async_write_eof();
   };
   test = [this, sent](Session session) -> awaitable<void>
   {
      auto [ec, request] = co_await session.async_submit(url, sent, as_tuple);
      logi("submit: {}", what(ec));
      if (ec)
         co_return;
      std::tie(ec) = co_await request.async_write_eof(as_tuple);
      logi("write_eof: {}", what(ec));
      auto [ec2, response] = co_await request.async_get_response(as_tuple);
      logi("get_response: {}", what(ec2));
      EXPECT_TRUE(ec2);
   };
}

TEST_P(Headers, WHEN_response_headers_exceed_limit_THEN_response_fails)
{
   if (GetParam() == anyhttp::Protocol::h3)
      GTEST_SKIP() << "no field section size limit for HTTP/3";

   auto sent = GetParam() == anyhttp::Protocol::http11 ? make_fields(1, 10_k) //
                                                       : make_fields(32, 32_k);
   custom = [sent](server::Request request, server::Response response) -> awaitable<void>
   {
      co_await drain(request);
      auto [ec] = co_await response.async_submit(200, sent, as_tuple);
      logi("server submit: {}", what(ec));
      if (!ec)
      {
         std::tie(ec) = co_await response.async_write_eof(as_tuple);
         logi("server write_eof: {}", what(ec));
      }
   };
   test = [this](Session session) -> awaitable<void>
   {
      auto request = co_await session.async_submit(url, {});
      co_await request.async_write_eof();
      auto [ec, response] = co_await request.async_get_response(as_tuple);
      logi("get_response: {}", what(ec));
      EXPECT_TRUE(ec);
   };
}
