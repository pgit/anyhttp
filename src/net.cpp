//
// The network half of the runtime layer, see anyhttp/net.hpp: what both runtimes do the same way,
// on the native handle. The rest is in asio/net.cpp and corosio/net.cpp.
//

#include "anyhttp/net.hpp"

#include <sys/socket.h>

// =================================================================================================
// UDP, the same in both runtimes, see anyhttp/net.hpp.
// =================================================================================================

// The runtime's own namespace, which io:: is part of: a plain "namespace anyhttp::io" here would
// be a namespace of its own to clang, which does not look through the inline namespace for it.
#if ANYHTTP_COROSIO
namespace anyhttp::inline corosio::io
#else
namespace anyhttp::inline asio::io
#endif
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

error_code send(UdpSocket& socket, boost::asio::const_buffer buffer) noexcept
{
   if (::send(socket.native_handle(), buffer.data(), buffer.size(), 0) < 0)
      return last_error();
   return {};
}

} // namespace anyhttp::io (in the runtime's namespace)
