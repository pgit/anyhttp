#pragma once

//
// std::formatter specializations for Boost.Asio types, see anyhttp/formatter.hpp.
//

#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>

#include <format>
#include <string_view>
#include <utility>

// =================================================================================================

/// "address:port", with an IPv6 address in square brackets.
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

// -------------------------------------------------------------------------------------------------

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
      auto append = [&](boost::asio::cancellation_type flag, std::string_view name) {
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
         std::format_to(ctx.out(), "{}0x{:x}", first ? "" : "|", std::to_underlying(type));

      return ctx.out();
   }
};

// =================================================================================================
