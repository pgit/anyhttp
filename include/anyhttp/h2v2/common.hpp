#pragma once

//
// Shared HTTP/2 building blocks, nghttp2 version 2. The counterpart of h2/common.hpp, with the
// same definitions in the same order, so that the two can be read side by side -- see H2_LIBRARY.
// This is the only place outside h2v2/ where <nghttp2v2/nghttp2.h> is pulled in; the generic
// server, client and formatter code knows nothing about nghttp2, whichever version.
//

#include <nghttp2v2/nghttp2.h>

#include <algorithm>
#include <chrono>
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
   return {reinterpret_cast<const uint8_t*>(name), reinterpret_cast<const uint8_t*>(value.data()),
           N - 1, value.size(), NGHTTP2_NV_FLAG_NO_COPY_NAME};
}

inline nghttp2_nv make_nv_ls(std::string_view key, std::string_view value)
{
   return {reinterpret_cast<const uint8_t*>(key.data()),
           reinterpret_cast<const uint8_t*>(value.data()), key.size(), value.size(),
           NGHTTP2_NV_FLAG_NONE};
}

/**
 * Version 2 counts and enforces the incoming field section itself, through
 * nghttp2_settings::max_field_section_size, so there is no CONTINUATION budget to compute here --
 * h2/common.hpp's max_continuations() has no counterpart.
 */

inline std::string_view name_of(const nghttp2_nv& nv)
{
   return {reinterpret_cast<const char*>(nv.name), nv.namelen};
}

inline std::string_view value_of(const nghttp2_nv& nv)
{
   return {reinterpret_cast<const char*>(nv.value), nv.valuelen};
}

// -------------------------------------------------------------------------------------------------

/// The payload of a reference-counted buffer, as the field callbacks deliver names and values.
inline std::string_view to_string_view(const nghttp2_vec& vec)
{
   return {reinterpret_cast<const char*>(vec.base), vec.len};
}

inline std::string_view to_string_view(const nghttp2_rcbuf* buf)
{
   return to_string_view(nghttp2_rcbuf_get_buf(buf));
}

// -------------------------------------------------------------------------------------------------

/**
 * Version 2 is driven by timestamps the way ngtcp2 is: nghttp2_conn_write() is told the current
 * time, and nghttp2_conn_get_expiry() asks to be called back at one. They are nanoseconds from an
 * arbitrary but monotonic origin, so a steady clock is what they have to come from.
 */
inline nghttp2_tstamp timestamp()
{
   using namespace std::chrono;
   return static_cast<nghttp2_tstamp>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

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
