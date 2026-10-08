#include "anyhttp/utils.hpp"

#include <cerrno>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// =================================================================================================

unsigned short get_unused_port()
{
   const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
   if (fd < 0)
      throw std::system_error(errno, std::generic_category(), "socket");

   sockaddr_in6 address{};
   address.sin6_family = AF_INET6;
   address.sin6_addr = in6addr_loopback;
   socklen_t size = sizeof(address);
   if (::bind(fd, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
       ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0)
   {
      const int error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "get_unused_port");
   }

   ::close(fd); // release immediately
   return ntohs(address.sin6_port);
}

// =================================================================================================
