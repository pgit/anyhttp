#pragma once

#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/http/field.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

// =================================================================================================

#define ENABLE_FMT_OSTREAM(X)                                                                      \
   template <>                                                                                     \
   struct std::formatter<X> : std::formatter<std::string>                                          \
   {                                                                                               \
      template <typename FormatContext>                                                            \
      auto format(const X& value, FormatContext& ctx) const                                        \
      {                                                                                            \
         std::ostringstream oss;                                                                   \
         oss << value;                                                                             \
         return std::formatter<std::string>::format(oss.str(), ctx);                               \
      }                                                                                            \
   };

ENABLE_FMT_OSTREAM(std::__thread_id);
ENABLE_FMT_OSTREAM(boost::urls::pct_string_view);
ENABLE_FMT_OSTREAM(boost::urls::authority_view);

#undef ENABLE_FMT_OSTREAM

// -------------------------------------------------------------------------------------------------

template <>
struct std::formatter<boost::core::string_view, char> : std::formatter<std::string_view, char>
{
   template <typename FormatContext>
   auto format(boost::core::string_view sv, FormatContext& ctx) const
   {
      auto temp = std::string_view{sv.data(), sv.size()};
      return std::formatter<std::string_view, char>::format(temp, ctx);
   }
};

// -------------------------------------------------------------------------------------------------

template <>
struct std::formatter<boost::asio::ip::tcp::endpoint>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <typename FormatContext>
   auto format(const boost::asio::ip::tcp::endpoint& endpoint, FormatContext& ctx) const
   {
      return std::format_to(ctx.out(), "{}:{}", endpoint.address().to_string(), endpoint.port());
   }
};

template <>
struct std::formatter<boost::beast::http::field>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <typename FormatContext>
   auto format(const boost::beast::http::field& field, FormatContext& ctx) const
   {
      return std::format_to(ctx.out(), "{}", to_string(field));
   }
};

// =================================================================================================
