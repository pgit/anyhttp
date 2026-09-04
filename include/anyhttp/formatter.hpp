#pragma once

#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include <boost/beast/http/field.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <thread>
#include <format>

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

template <class Proto>
struct std::formatter<boost::asio::ip::basic_endpoint<Proto>>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <typename FormatContext>
   auto format(const boost::asio::ip::basic_endpoint<Proto>& endpoint, FormatContext& ctx) const
   {
      const auto address = endpoint.address();
      if (address.is_v6())
         return std::format_to(ctx.out(), "[{}]:{}", address.to_string(), endpoint.port());
      else
         return std::format_to(ctx.out(), "{}:{}", address.to_string(), endpoint.port());
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
      using enum boost::asio::cancellation_type;

      if (type == none)
         return std::formatter<std::string_view>::format("none", ctx);

      if (type == all)
         return std::formatter<std::string_view>::format("all", ctx);

      bool first = true;
      auto append = [&](boost::asio::cancellation_type flag, std::string_view name)
      {
         if ((type & flag) == flag)
         {
            std::format_to(ctx.out(), "{}{}", first ? "" : "|", name);
            first = false;
            type = type & ~flag;
         }
      };

      append(terminal, "terminal");
      append(partial, "partial");
      append(total, "total");

      if (type != none)
         std::format_to(ctx.out(), "{}0x{:x}", first ? "" : "|", to_underlying(type));

      return ctx.out();
   }
};

// =================================================================================================
