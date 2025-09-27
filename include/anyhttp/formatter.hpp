#pragma once

#include <nghttp2/nghttp2.h>

#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/http/field.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <thread>

#include <format>
#include <ranges>

namespace rv = std::ranges::views;

// =================================================================================================

using thread_id = decltype(std::this_thread::get_id());

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

ENABLE_FMT_OSTREAM(thread_id);
ENABLE_FMT_OSTREAM(boost::urls::pct_string_view);
ENABLE_FMT_OSTREAM(boost::urls::authority_view);

#undef ENABLE_FMT_OSTREAM

// -------------------------------------------------------------------------------------------------

template <>
struct std::formatter<boost::core::string_view> : public std::formatter<std::string_view>
{
   template <typename FormatContext>
   constexpr auto format(boost::core::string_view sv, FormatContext& ctx) const
   {
      return std::formatter<std::string_view>::format(std::string_view{sv.data(), sv.size()}, ctx);
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

template <>
struct std::formatter<boost::asio::cancellation_type> : std::formatter<std::string_view>
{
   auto format(boost::asio::cancellation_type type, auto& ctx) const
   {
      using ct = boost::asio::cancellation_type;
      using enum ct;

      if (type == none)
         return std::formatter<std::string_view>::format("none", ctx);

      if (type == all)
         return std::formatter<std::string_view>::format("all", ctx);

      bool first = true;
      auto append_if = [&](boost::asio::cancellation_type flag, std::string_view name)
      {
         if ((type & flag) == flag)
         {
            std::format_to(ctx.out(), "{}{}", first ? "" : "|", name);
            first = false;
            type = type & ~flag;
         }
      };

      append_if(terminal, "terminal");
      append_if(partial, "partial");
      append_if(total, "total");

      if (type != none)
         std::format_to(ctx.out(), "{}0x{:x}", first ? "" : "|", to_underlying(type));

      return ctx.out();
   }
};

// =================================================================================================

template <>
struct std::formatter<nghttp2_nv>
{
   enum class part
   {
      name_and_value,
      name,
      value
   } what = part::name_and_value;

   constexpr auto parse(std::format_parse_context& ctx)
   {
      auto it = ctx.begin();
      if (it == ctx.end())
         return it;

      if (*it == 'n')
      {
         what = part::name;
         ++it;
      }
      else if (*it == 'v')
      {
         what = part::value;
         ++it;
      }

      if (it != ctx.end() && *it != '}')
         throw std::format_error("invalid format args for nghttp2_nv, expected 'n' or 'v'");

      return it;
   }

   auto format(const nghttp2_nv& nv, std::format_context& ctx) const
   {
      auto out = ctx.out();
      if (what == part::name || what == part::name_and_value)
         std::ranges::copy(rv::counted(nv.name, nv.namelen), out);
      if (what == part::name_and_value)
         *out++ = '=';
      if (what == part::value || what == part::name_and_value)
         std::ranges::copy(rv::counted(nv.value, nv.valuelen), out);
      return out;
   }
};

// =================================================================================================