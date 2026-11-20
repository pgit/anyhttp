//
// Boost.HTTP's parser on its own, for what anyhttp needs of it that its tests do not cover, and for
// the patches cmake/http.cmake applies to it.
//
#include "anyhttp/h1/io.hpp"

#include <boost/http/request_parser.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>

using namespace testing;

namespace http = boost::http;

// =================================================================================================

class H1Parser : public Test
{
protected:
   H1Parser() : parser(anyhttp::h1::make_parser_config(true, 16 * 1024))
   {
      parser.reset();
      parser.start();
   }

   /// Gives the parser \p input, as one read would, and parses it.
   std::error_code put(std::string_view input)
   {
      auto buffers = parser.prepare();
      EXPECT_GE(buffers[0].size(), input.size());
      input.copy(static_cast<char*>(buffers[0].data()), input.size());
      parser.commit(input.size());
      std::error_code ec;
      parser.parse(ec);
      if (parser.got_header() && !parser.is_complete())
         parser.parse(ec); // the body, which the header stops short of
      return ec;
   }

   http::request_parser parser;
};

// -------------------------------------------------------------------------------------------------

//
// The h2c upgrade (h1/session.cpp) hands what follows the request to HTTP/2.
// parser::release_buffered_data() is a stub on Boost.HTTP's 'develop', see
// cmake/patches/http-release-buffered-data.patch.
//

TEST_F(H1Parser, WHEN_data_follows_a_request_without_body_THEN_it_is_released)
{
   put("GET / HTTP/1.1\r\nHost: x\r\n\r\nPRI * HTTP/2.0\r\n");
   ASSERT_TRUE(parser.is_complete());
   ASSERT_TRUE(parser.has_buffered_data());

   EXPECT_EQ(parser.release_buffered_data(), "PRI * HTTP/2.0\r\n");
   EXPECT_FALSE(parser.has_buffered_data()) << "released, so the next message does not see it";
   EXPECT_EQ(parser.release_buffered_data(), "");
}

TEST_F(H1Parser, WHEN_data_follows_a_body_THEN_only_that_is_released_and_body_stays)
{
   put("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloworld");
   ASSERT_TRUE(parser.is_complete());

   EXPECT_EQ(parser.release_buffered_data(), "world");
   EXPECT_EQ(parser.body(), "hello");
}

TEST_F(H1Parser, WHEN_nothing_follows_THEN_nothing_is_released)
{
   put("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
   ASSERT_TRUE(parser.is_complete());
   EXPECT_FALSE(parser.has_buffered_data());
   EXPECT_EQ(parser.release_buffered_data(), "");
}

TEST_F(H1Parser, WHEN_message_is_incomplete_THEN_nothing_is_released)
{
   put("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel");
   ASSERT_FALSE(parser.is_complete());
   EXPECT_EQ(parser.release_buffered_data(), "");
}

// -------------------------------------------------------------------------------------------------

//
// The parser's limits on the header section all come out as errors::header_limit, see
// h1::make_parser_config() and h1::to_error_code().
//

TEST_F(H1Parser, WHEN_a_single_field_is_larger_than_the_old_limit_THEN_it_is_fine)
{
   // Boost.HTTP's default for a single field is 4 KiB; anyhttp's limit is on the whole section
   std::string request = "GET / HTTP/1.1\r\nX-Large: " + std::string(8000, 'a') + "\r\n\r\n";
   EXPECT_FALSE(put(request));
   EXPECT_TRUE(parser.is_complete());
}

TEST_F(H1Parser, WHEN_there_are_many_small_fields_THEN_they_count_against_the_total)
{
   std::string request = "GET / HTTP/1.1\r\n";
   for (int i = 0; request.size() < 15 * 1024; ++i)
      request += std::format("x-{}: 1\r\n", i);
   auto ec = put(request + "\r\n");
   EXPECT_EQ(anyhttp::h1::to_error_code(ec), anyhttp::errors::header_limit) << ec.message();
}

// =================================================================================================
