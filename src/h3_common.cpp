//
// Small helpers shared by the HTTP/3 server and client, see anyhttp/h3_common.hpp.
//
#include "anyhttp/h3_common.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdarg>
#include <cstdio>

namespace anyhttp::http3
{

// =================================================================================================

nghttp3_nv make_nv(std::string_view name, std::string_view value)
{
   nghttp3_nv nv{};
   nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
   nv.namelen = name.size();
   nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
   nv.valuelen = value.size();
   nv.flags = NGHTTP3_NV_FLAG_NONE;
   return nv;
}

void log_headers(std::string_view log_prefix, std::span<const nghttp3_nv> nva)
{
   for (const auto& nv : nva)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", log_prefix,
           std::string_view(reinterpret_cast<const char*>(nv.name), nv.namelen),
           std::string_view(reinterpret_cast<const char*>(nv.value), nv.valuelen));
}

void log_headers(std::string_view log_prefix,
                 const std::vector<std::pair<std::string, std::string>>& headers)
{
   for (const auto& [name, value] : headers)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", log_prefix, name, value);
}

void ngtcp2_log_printf(void* /*user*/, const char* fmt, ...) noexcept
{
   if (!spdlog::default_logger()->should_log(spdlog::level::trace))
      return;
   std::array<char, 512> buf;
   va_list ap;
   va_start(ap, fmt);
   std::vsnprintf(buf.data(), buf.size(), fmt, ap);
   va_end(ap);
   spdlog::trace("{}", buf.data());
}

// =================================================================================================

} // namespace anyhttp::http3
