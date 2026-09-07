//
// Small helpers shared by the HTTP/3 server and client, see anyhttp/h3_common.hpp.
//
#include "anyhttp/h3_common.hpp"
#include "anyhttp/common.hpp" // IWYU pragma: keep

#include <spdlog/spdlog.h>

#include <netdb.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <format>

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
//
// Below: helpers taken from the ngtcp2 examples, see anyhttp/h3_common.hpp.
//

std::optional<Address> msghdr_get_local_addr(msghdr* msg, int family)
{
   switch (family)
   {
   case AF_INET:
      for (auto cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg))
      {
         if (cmsg->cmsg_level != IPPROTO_IP || cmsg->cmsg_type != IP_PKTINFO)
            continue;

         in_pktinfo pktinfo;
         std::memcpy(&pktinfo, CMSG_DATA(cmsg), sizeof(pktinfo));
         Address res{.len = sizeof(res.su.in),
                     .ifindex = static_cast<uint32_t>(pktinfo.ipi_ifindex)};
         res.su.in.sin_family = AF_INET;
         res.su.in.sin_addr = pktinfo.ipi_addr;
         return res;
      }
      return {};

   case AF_INET6:
      for (auto cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg))
      {
         if (cmsg->cmsg_level != IPPROTO_IPV6 || cmsg->cmsg_type != IPV6_PKTINFO)
            continue;

         in6_pktinfo pktinfo;
         std::memcpy(&pktinfo, CMSG_DATA(cmsg), sizeof(pktinfo));
         Address res{.len = sizeof(res.su.in6),
                     .ifindex = static_cast<uint32_t>(pktinfo.ipi6_ifindex)};
         res.su.in6.sin6_family = AF_INET6;
         res.su.in6.sin6_addr = pktinfo.ipi6_addr;
         return res;
      }
      return {};
   }

   return {};
}

void set_port(Address& dst, const Address& src)
{
   switch (dst.su.storage.ss_family)
   {
   case AF_INET:
      assert(AF_INET == src.su.storage.ss_family);
      dst.su.in.sin_port = src.su.in.sin_port;
      return;

   case AF_INET6:
      assert(AF_INET6 == src.su.storage.ss_family);
      dst.su.in6.sin6_port = src.su.in6.sin6_port;
      return;

   default:
      assert(0);
   }
}

ngtcp2_tstamp timestamp()
{
   using namespace std::chrono;
   return static_cast<ngtcp2_tstamp>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string straddr(const sockaddr* sa, socklen_t salen)
{
   std::array<char, NI_MAXHOST> host;
   std::array<char, NI_MAXSERV> port;

   auto rv = getnameinfo(sa, salen, host.data(), host.size(), port.data(), port.size(),
                         NI_NUMERICHOST | NI_NUMERICSERV);
   if (rv != 0)
   {
      loge("getnameinfo: {}", gai_strerror(rv));
      return {};
   }

   return std::format("[{}]:{}", host.data(), port.data());
}

std::string format_hex(const uint8_t* data, size_t len)
{
   constexpr char xdigits[] = "0123456789abcdef";

   std::string res;
   res.reserve(len * 2);
   for (size_t i = 0; i < len; ++i)
   {
      res += xdigits[data[i] >> 4];
      res += xdigits[data[i] & 0xf];
   }

   return res;
}

// =================================================================================================

} // namespace anyhttp::http3
