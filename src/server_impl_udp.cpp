#include "anyhttp/server_impl.hpp"
#include "anyhttp/literals.hpp"

#include "anyhttp/formatter.hpp" // IWYU pragma: keep

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/error.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <netinet/udp.h>

#include <print>

#define IPTOS_ECN_MASK 0x03

#include "ngtcp2/shared.h"

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

awaitable<void> Server::Impl::udp_receive_loop()
{
   std::array<uint8_t, 64 * 1024> buf;

   struct iovec iov{buf.data(), buf.size()};
   struct sockaddr_storage sender_addr;
   auto family = sender_addr.ss_family;
   struct msghdr msg{};
   msg.msg_name = &sender_addr;
   msg.msg_namelen = sizeof(sender_addr);
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   std::array<uint8_t, CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int))> control_data;
   msg.msg_control = control_data.data();
   msg.msg_controllen = control_data.size();

   auto native_handle = m_udp_socket->native_handle();
   for (;;)
   {
      co_await m_udp_socket->async_wait(boost::asio::socket_base::wait_read);

      auto nread = recvmsg(native_handle, &msg, 0);
      logd("{} bytes from={} ecn={}", nread, normalize(sockaddr_to_endpoint(sender_addr)),
           ngtcp2::msghdr_get_ecn(&msg, family));

      for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
           cmsg = CMSG_NXTHDR(&msg, cmsg))
      {
         if (cmsg->cmsg_level == IPPROTO_IP)
         {
            if (cmsg->cmsg_type == IP_TOS)
            {
               int tos = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
               std::println("Received TOS: {:x}", tos);
            }
            else if (cmsg->cmsg_type == IP_TTL)
            {
               int ttl = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
               std::println("Received TTL: {}", ttl);
            }
         }
         else if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO)
         {
            int gso_size = 0;
            memcpy(&gso_size, CMSG_DATA(cmsg), sizeof(gso_size));
            std::println("Received UDP GRO {}", gso_size);
            break;
         }
      }
   }
}

// =================================================================================================

} // namespace anyhttp::server
