//
// The network half of the runtime layer, see anyhttp/net.hpp: what both runtimes do the same way,
// on the native handle. The rest is in net_asio.cpp and net_corosio.cpp.
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

error_code bind(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept
{
   if (::bind(socket.native_handle(), endpoint.data(), static_cast<socklen_t>(endpoint.size())))
      return last_error();
   return {};
}

error_code connect(UdpSocket& socket, const asio::ip::udp::endpoint& endpoint) noexcept
{
   if (::connect(socket.native_handle(), endpoint.data(), static_cast<socklen_t>(endpoint.size())))
      return last_error();
   return {};
}

asio::ip::udp::endpoint local_endpoint(UdpSocket& socket)
{
   asio::ip::udp::endpoint endpoint;
   auto size = static_cast<socklen_t>(endpoint.capacity());
   if (::getsockname(socket.native_handle(), endpoint.data(), &size))
      throw_error(last_error());
   endpoint.resize(size);
   return endpoint;
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
