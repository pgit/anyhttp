/*
 * ngtcp2
 *
 * Copyright (c) 2019 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shared.h"

#include <cassert>
#include <cstring>

namespace ngtcp2 {

std::optional<Address> msghdr_get_local_addr(msghdr *msg, int family) {
  switch (family) {
  case AF_INET:
    for (auto cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
        in_pktinfo pktinfo;
        memcpy(&pktinfo, CMSG_DATA(cmsg), sizeof(pktinfo));
        Address res{
          .len = sizeof(res.su.in),
          .ifindex = static_cast<uint32_t>(pktinfo.ipi_ifindex),
        };
        auto &sa = res.su.in;
        sa.sin_family = AF_INET;
        sa.sin_addr = pktinfo.ipi_addr;
        return res;
      }
    }
    return {};
  case AF_INET6:
    for (auto cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
        in6_pktinfo pktinfo;
        memcpy(&pktinfo, CMSG_DATA(cmsg), sizeof(pktinfo));
        Address res{
          .len = sizeof(res.su.in6),
          .ifindex = static_cast<uint32_t>(pktinfo.ipi6_ifindex),
        };
        auto &sa = res.su.in6;
        sa.sin6_family = AF_INET6;
        sa.sin6_addr = pktinfo.ipi6_addr;
        return res;
      }
    }
    return {};
  }
  return {};
}

void set_port(Address &dst, const Address &src) {
  switch (dst.su.storage.ss_family) {
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

} // namespace ngtcp2
