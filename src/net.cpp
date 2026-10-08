//
// The network half of the runtime layer, see anyhttp/net.hpp: what both runtimes do the same way,
// on the native handle. The rest is in asio/net.cpp and corosio/net.cpp.
//

#include "anyhttp/net.hpp"

#include <sys/socket.h>

namespace anyhttp
{

// =================================================================================================
// UDP, the same in both runtimes, see anyhttp/net.hpp.
// =================================================================================================

namespace io
{

error_code bind(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept
{
   const auto address = to_sockaddr(endpoint);
   if (::bind(socket.native_handle(), address.data(), address.size))
      return last_error();
   return {};
}

error_code connect(UdpSocket& socket, const UdpEndpoint& endpoint) noexcept
{
   const auto address = to_sockaddr(endpoint);
   if (::connect(socket.native_handle(), address.data(), address.size))
      return last_error();
   return {};
}

UdpEndpoint local_endpoint(UdpSocket& socket)
{
   SocketAddress address;
   address.size = sizeof(address.storage);
   if (::getsockname(socket.native_handle(), address.data(), &address.size))
      throw_error(last_error());
   auto endpoint = from_sockaddr(address.data(), address.size);
   if (!endpoint)
      throw_error(make_error_code(errc::address_family_not_supported));
   return *endpoint;
}

error_code set_option(UdpSocket& socket, int level, int name, int value) noexcept
{
   if (::setsockopt(socket.native_handle(), level, name, &value, sizeof(value)))
      return last_error();
   return {};
}

error_code send(UdpSocket& socket, asio::const_buffer buffer) noexcept
{
   if (::send(socket.native_handle(), buffer.data(), buffer.size(), 0) < 0)
      return last_error();
   return {};
}

} // namespace io

// =================================================================================================

} // namespace anyhttp
