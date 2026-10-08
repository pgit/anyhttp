#pragma once

#include "anyhttp/config.hpp"

#if ANYHTTP_COROSIO
#include "anyhttp/corosio/formatter.hpp"
#else
#include "anyhttp/asio/formatter.hpp"
#endif

#include <boost/beast/http/field.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <cstddef>
#include <format>
#include <sstream>
#include <string_view>
#include <thread>

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

namespace anyhttp
{

/// A string to be logged, cut short if it is longer than \c max_size bytes. See truncated().
struct Truncated
{
   std::string_view text;
   size_t max_size;
};

/// Default for truncated(): long enough for any regular header, short enough to keep the log
/// readable.
inline constexpr size_t max_logged_size = 80;

/**
 * Wraps \p text for logging, so that only its first \p max_size bytes are printed, followed by the
 * total size. Meant for header names and values, which may be of almost any size:
 * \code
 * logd("{}: {}", truncated(name), truncated(value)); // x-large: aaaa... (30000 bytes, truncated)
 * \endcode
 */
inline Truncated truncated(std::string_view text, size_t max_size = max_logged_size)
{
   return {text, max_size};
}

} // namespace anyhttp

template <>
struct std::formatter<anyhttp::Truncated> : std::formatter<std::string_view>
{
   auto format(const anyhttp::Truncated& value, std::format_context& ctx) const
   {
      if (value.text.size() <= value.max_size)
         return std::formatter<std::string_view>::format(value.text, ctx);

      return std::format_to(ctx.out(), "{}... ({} bytes, truncated)",
                            value.text.substr(0, value.max_size), value.text.size());
   }
};
