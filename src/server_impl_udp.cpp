#include "anyhttp/literals.hpp"
#include "anyhttp/server_impl.hpp"

#include "anyhttp/formatter.hpp" // IWYU pragma: keep

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/detail/engine.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <cstring>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <netinet/udp.h>

#include <print>

#define IPTOS_ECN_MASK 0x03

#include "ngtcp2/shared.h"
#include "ngtcp2/util.h"

using namespace std::chrono_literals;
using namespace boost::asio;

namespace anyhttp::server
{

// =================================================================================================

boost::asio::ip::tcp::endpoint sockaddr_to_endpoint(const sockaddr_storage& addr)
{
   using namespace boost::asio::ip;
   if (addr.ss_family == AF_INET)
   {
      const auto& sa = reinterpret_cast<const sockaddr_in&>(addr);
      return tcp::endpoint(make_address_v4(ntohl(sa.sin_addr.s_addr)), ntohs(sa.sin_port));
   }
   else if (addr.ss_family == AF_INET6)
   {
      const auto& sa6 = reinterpret_cast<const sockaddr_in6&>(addr);
      const auto& bytes = reinterpret_cast<const address_v6::bytes_type&>(sa6.sin6_addr);
      return tcp::endpoint(make_address_v6(bytes, sa6.sin6_scope_id), ntohs(sa6.sin6_port));
   }
   else
   {
      throw std::invalid_argument("Unsupported address family");
   }
}

// -------------------------------------------------------------------------------------------------

using namespace ngtcp2;

// Endpoint is a local endpoint.
struct Endpoint
{
   Address addr;
   int fd;
};

int Server::Impl::udp_on_read(Endpoint& ep)
{
#if 1
   sockaddr_union su;
   std::array<uint8_t, 64_k> buf;
   size_t pktcnt = 0;
   ngtcp2_pkt_info pi;

   iovec msg_iov;
   msg_iov.iov_base = buf.data();
   msg_iov.iov_len = buf.size();

   msghdr msg{};
   msg.msg_name = &su;
   msg.msg_iov = &msg_iov;
   msg.msg_iovlen = 1;

   uint8_t
      msg_ctrl[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(int))];
   msg.msg_control = msg_ctrl;

   for ( ;pktcnt < 10; )
   {
      msg.msg_namelen = sizeof(su);
      msg.msg_controllen = sizeof(msg_ctrl);

      auto nread = recvmsg(ep.fd, &msg, 0);
      if (nread == -1)
      {
         if (!(errno == EAGAIN || errno == ENOTCONN))
         {
            std::cerr << "recvmsg: " << strerror(errno) << std::endl;
         }
         return 0;
      }

      // Packets less than 22 bytes never be a valid QUIC packet.
      if (nread < 22)
      {
         ++pktcnt;
         continue;
      }

      if (util::prohibited_port(util::port(&su)))
      {
         ++pktcnt;
         continue;
      }

      pi.ecn = msghdr_get_ecn(&msg, su.storage.ss_family);
      auto local_addr = msghdr_get_local_addr(&msg, su.storage.ss_family);
      if (!local_addr)
      {
         ++pktcnt;
         std::cerr << "Unable to obtain local address" << std::endl;
         continue;
      }

      auto gso_size = msghdr_get_udp_gro(&msg);
      if (gso_size == 0)
      {
         gso_size = static_cast<size_t>(nread);
      }

      set_port(*local_addr, ep.addr);

      auto data = std::span{buf.data(), static_cast<size_t>(nread)};

      for (; !data.empty();)
      {
         auto datalen = std::min(data.size(), gso_size);

         ++pktcnt;

         if (true /* !config.quiet */)
         {
            std::array<char, IF_NAMESIZE> ifname;
            std::cerr << "Received packet: local="
                      << util::straddr(&local_addr->su.sa, local_addr->len)
                      << " remote=" << util::straddr(&su.sa, msg.msg_namelen)
                      << " if=" << if_indextoname(local_addr->ifindex, ifname.data()) << " ecn=0x"
                      << std::hex << static_cast<uint32_t>(pi.ecn) << std::dec << " " << datalen
                      << " bytes" << std::endl;
         }

         // Packets less than 22 bytes never be a valid QUIC packet.
         if (datalen < 22)
         {
            break;
         }

         // read_pkt(ep, *local_addr, &su.sa, msg.msg_namelen, &pi, {data.data(), datalen});

         data = data.subspan(datalen);
      }
   }
#endif
   return 0;
}

awaitable<void> Server::Impl::udp_receive_loop()
{
   for (;;)
   {
      co_await m_udp_socket->async_wait(boost::asio::socket_base::wait_read);

      Endpoint ep;
      ep.fd = m_udp_socket->native_handle();
      ep.addr.su.sa = *m_udp_socket->local_endpoint().data();
      udp_on_read(ep);
   }
}

// =================================================================================================

} // namespace anyhttp::server
