//
// The receive window a TCP socket has advertised last. Linux reports it in TCP_INFO, but glibc's
// struct tcp_info ends before that field, and the kernel's, in <linux/tcp.h>, cannot share a
// translation unit with <netinet/tcp.h>, which Asio includes. Hence a file of its own.
//
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace anyhttp
{

/// The receive window that socket \p fd has advertised last, in bytes, if the kernel says.
std::optional<uint32_t> advertised_receive_window(int fd)
{
   tcp_info info{};
   socklen_t size = sizeof(info);
   if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size) != 0 ||
       size < offsetof(tcp_info, tcpi_rcv_wnd) + sizeof(info.tcpi_rcv_wnd))
      return std::nullopt;
   return info.tcpi_rcv_wnd;
}

} // namespace anyhttp
