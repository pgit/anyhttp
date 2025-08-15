#pragma once

#include <nghttp2/nghttp2.h>

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

// =================================================================================================

} // namespace anyhttp
