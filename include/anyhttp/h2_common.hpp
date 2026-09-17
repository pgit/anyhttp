#pragma once

//
// Shared HTTP/2 building blocks: the bits of nghttp2 glue that more than one of the h2_* files
// needs. This is the only place outside them where <nghttp2/nghttp2.h> is pulled in -- the
// generic server, client and formatter code knows nothing about nghttp2.
//

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>

namespace anyhttp
{

// =================================================================================================

// Create nghttp2_nv from string literal |name| and std::string |value|.
// FIXME: don't use this, it is dangerous (prone to dangling string references)
template <size_t N>
nghttp2_nv make_nv_ls(const char (&name)[N], std::string_view value)
{
   return {(uint8_t*)name, (uint8_t*)value.data(), N - 1, value.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME};
}

inline nghttp2_nv make_nv_ls(std::string_view key, std::string_view value)
{
   return {(uint8_t*)key.data(), (uint8_t*)value.data(), key.size(), value.size(), 0};
}

/**
 * The number of CONTINUATION frames to accept after a HEADERS frame, for a header section of up to
 * \p max_header_size bytes in frames of the default size. A peer sending more is flooding us and
 * loses the connection. nghttp2 accepts 8 by default, which is kept as the minimum.
 */
inline size_t max_continuations(size_t max_header_size)
{
   return std::max<size_t>(8, max_header_size / 16384 + 1);
}

inline std::string_view name_of(const nghttp2_nv& nv) { return {(const char*)nv.name, nv.namelen}; }
inline std::string_view value_of(const nghttp2_nv& nv) { return {(const char*)nv.value, nv.valuelen}; }

// =================================================================================================

} // namespace anyhttp

// =================================================================================================

/// Formats an HTTP/2 name-value pair (nghttp2_nv).
/// Format specifiers: 'n' = name only, 'v' = value only, default = "name=value".
/// Example: {:n} → "content-type", {:v} → "application/json", {} → "content-type=application/json"
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
      namespace rv = std::ranges::views;

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
