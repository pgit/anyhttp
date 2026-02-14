#include <gtest/gtest.h>
#include <anyhttp/formatter.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>
#include <nghttp2/nghttp2.h>

#include <format>
#include <thread>

// =================================================================================================
// Test thread_id formatter
// =================================================================================================

TEST(FormatterTest, ThreadId)
{
   auto tid = std::this_thread::get_id();
   auto formatted = std::format("{}", tid);
   EXPECT_FALSE(formatted.empty());
}

// =================================================================================================
// Test boost::urls::pct_string_view formatter
// =================================================================================================

TEST(FormatterTest, PctStringView)
{
   boost::urls::pct_string_view psv("hello%20world");
   auto formatted = std::format("{}", psv);
   EXPECT_EQ(formatted, "hello%20world");
}

// =================================================================================================
// Test boost::urls::authority_view formatter
// =================================================================================================

TEST(FormatterTest, AuthorityView)
{
   boost::urls::authority_view av("user:pass@example.com:8080");
   auto formatted = std::format("{}", av);
   EXPECT_EQ(formatted, "user:pass@example.com:8080");
}

TEST(FormatterTest, AuthorityViewSimple)
{
   boost::urls::authority_view av("example.com");
   auto formatted = std::format("{}", av);
   EXPECT_EQ(formatted, "example.com");
}

// =================================================================================================
// Test boost::core::string_view formatter
// =================================================================================================

TEST(FormatterTest, BoostStringView)
{
   boost::core::string_view sv("test string");
   auto formatted = std::format("{}", sv);
   EXPECT_EQ(formatted, "test string");
}

TEST(FormatterTest, BoostStringViewEmpty)
{
   boost::core::string_view sv("");
   auto formatted = std::format("{}", sv);
   EXPECT_EQ(formatted, "");
}

// =================================================================================================
// Test boost::asio::ip::tcp::endpoint formatter
// =================================================================================================

TEST(FormatterTest, EndpointIPv4)
{
   auto addr = boost::asio::ip::address_v4::from_string("192.168.1.1");
   boost::asio::ip::tcp::endpoint endpoint(addr, 8080);
   auto formatted = std::format("{}", endpoint);
   EXPECT_EQ(formatted, "192.168.1.1:8080");
}

TEST(FormatterTest, EndpointIPv6)
{
   auto addr = boost::asio::ip::address_v6::from_string("::1");
   boost::asio::ip::tcp::endpoint endpoint(addr, 9090);
   auto formatted = std::format("{}", endpoint);
   EXPECT_EQ(formatted, "[::1]:9090");
}

TEST(FormatterTest, EndpointIPv6Full)
{
   auto addr = boost::asio::ip::address_v6::from_string("2001:db8::1");
   boost::asio::ip::tcp::endpoint endpoint(addr, 443);
   auto formatted = std::format("{}", endpoint);
   EXPECT_EQ(formatted, "[2001:db8::1]:443");
}

// =================================================================================================
// Test boost::beast::http::field formatter
// =================================================================================================

TEST(FormatterTest, HttpField)
{
   auto field = boost::beast::http::field::content_type;
   auto formatted = std::format("{}", field);
   EXPECT_EQ(formatted, "Content-Type");
}

TEST(FormatterTest, HttpFieldAccept)
{
   auto field = boost::beast::http::field::accept;
   auto formatted = std::format("{}", field);
   EXPECT_EQ(formatted, "Accept");
}

TEST(FormatterTest, HttpFieldUserAgent)
{
   auto field = boost::beast::http::field::user_agent;
   auto formatted = std::format("{}", field);
   EXPECT_EQ(formatted, "User-Agent");
}

// =================================================================================================
// Test boost::asio::cancellation_type formatter
// =================================================================================================

TEST(FormatterTest, CancellationTypeNone)
{
   auto ct = boost::asio::cancellation_type::none;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "none");
}

TEST(FormatterTest, CancellationTypeAll)
{
   auto ct = boost::asio::cancellation_type::all;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "all");
}

TEST(FormatterTest, CancellationTypeTerminal)
{
   auto ct = boost::asio::cancellation_type::terminal;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "terminal");
}

TEST(FormatterTest, CancellationTypePartial)
{
   auto ct = boost::asio::cancellation_type::partial;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "partial");
}

TEST(FormatterTest, CancellationTypeTotal)
{
   auto ct = boost::asio::cancellation_type::total;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "total");
}

TEST(FormatterTest, CancellationTypeCombined)
{
   auto ct = boost::asio::cancellation_type::terminal | boost::asio::cancellation_type::partial;
   auto formatted = std::format("{}", ct);
   // The order is: terminal, partial, total
   EXPECT_EQ(formatted, "terminal|partial");
}

TEST(FormatterTest, CancellationTypeMultipleCombined)
{
   auto ct = boost::asio::cancellation_type::terminal | boost::asio::cancellation_type::total;
   auto formatted = std::format("{}", ct);
   EXPECT_EQ(formatted, "terminal|total");
}

// =================================================================================================
// Test nghttp2_nv formatter
// =================================================================================================

TEST(FormatterTest, NgHttp2NvDefault)
{
   const char* name = "content-type";
   const char* value = "text/html";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      strlen(name),
      strlen(value),
      NGHTTP2_NV_FLAG_NONE
   };
   
   auto formatted = std::format("{}", nv);
   EXPECT_EQ(formatted, "content-type=text/html");
}

TEST(FormatterTest, NgHttp2NvNameOnly)
{
   const char* name = "accept-encoding";
   const char* value = "gzip, deflate";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      strlen(name),
      strlen(value),
      NGHTTP2_NV_FLAG_NONE
   };
   
   auto formatted = std::format("{:n}", nv);
   EXPECT_EQ(formatted, "accept-encoding");
}

TEST(FormatterTest, NgHttp2NvValueOnly)
{
   const char* name = "user-agent";
   const char* value = "Mozilla/5.0";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      strlen(name),
      strlen(value),
      NGHTTP2_NV_FLAG_NONE
   };
   
   auto formatted = std::format("{:v}", nv);
   EXPECT_EQ(formatted, "Mozilla/5.0");
}

TEST(FormatterTest, NgHttp2NvInvalidFormat)
{
   const char* name = "test";
   const char* value = "value";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      strlen(name),
      strlen(value),
      NGHTTP2_NV_FLAG_NONE
   };
   
   // Invalid format specifier should throw
   EXPECT_THROW(std::format("{:x}", nv), std::format_error);
}

TEST(FormatterTest, NgHttp2NvEmptyName)
{
   const char* name = "";
   const char* value = "some-value";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      0,
      strlen(value),
      NGHTTP2_NV_FLAG_NONE
   };
   
   auto formatted = std::format("{}", nv);
   EXPECT_EQ(formatted, "=some-value");
}

TEST(FormatterTest, NgHttp2NvEmptyValue)
{
   const char* name = "some-header";
   const char* value = "";
   nghttp2_nv nv = {
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name)),
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value)),
      strlen(name),
      0,
      NGHTTP2_NV_FLAG_NONE
   };
   
   auto formatted = std::format("{}", nv);
   EXPECT_EQ(formatted, "some-header=");
}
