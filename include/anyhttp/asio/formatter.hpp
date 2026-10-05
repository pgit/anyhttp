#pragma once

//
// std::formatter specializations for Boost.Asio types, see anyhttp/formatter.hpp.
//

#include <boost/asio/cancellation_type.hpp>

#include <format>
#include <string_view>
#include <utility>

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
