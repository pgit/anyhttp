#pragma once

//
// std::formatter specializations for corosio types, see anyhttp/formatter.hpp.
//

#include <boost/corosio/endpoint.hpp>

#include <format>

// =================================================================================================

/// "address:port", with an IPv6 address in square brackets, as with ASIO.
template <>
struct std::formatter<boost::corosio::endpoint>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <typename FormatContext>
   auto format(const boost::corosio::endpoint& endpoint, FormatContext& ctx) const
   {
      const auto address = endpoint.address();
      if (address.is_v6())
         return std::format_to(ctx.out(), "[{}]:{}", address.to_string(), endpoint.port());
      else
         return std::format_to(ctx.out(), "{}:{}", address.to_string(), endpoint.port());
   }
};

// =================================================================================================
