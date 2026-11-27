//
// The network half of the runtime layer, see anyhttp/net.hpp: what both runtimes do the same way,
// on the native handle. The rest is in asio/net.cpp and corosio/net.cpp.
//

#include "anyhttp/net.hpp"

#include <net/if.h>
#include <sys/socket.h>

#include <string_view>
#include <tuple>

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

error_code bind_to_device(UdpSocket& socket, std::string_view device) noexcept
{
#ifdef SO_BINDTODEVICE
   //
   // The kernel takes the name up to its first NUL, and at most IFNAMSIZ - 1 characters of it,
   // without telling: a longer one would bind to whatever interface has the name cut short.
   //
   if (device.empty() || device.size() >= IFNAMSIZ || device.find('\0') != device.npos)
      return make_error_code(errc::no_such_device);
   if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_BINDTODEVICE, device.data(),
                    static_cast<socklen_t>(device.size())))
      return last_error();
   return {};
#else
   std::ignore = socket;
   std::ignore = device;
   return make_error_code(errc::operation_not_supported);
#endif
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

// =================================================================================================
// Addresses, see anyhttp/net.hpp.
// =================================================================================================

std::optional<IpAddress> parse_address(std::string_view text, error_code& ec) noexcept
{
   ec = {};
   if (text.empty() || text == "*")
      return std::nullopt;
   auto address = make_address(text, ec);
   if (ec)
   {
      ec = make_error_code(errc::invalid_argument); // whatever each runtime says to it
      return std::nullopt;
   }
   return normalize(address);
}

std::optional<IpAddress> bind_address(const std::optional<IpAddress>& local,
                                      const IpAddress& remote) noexcept
{
   if (!local)
   {
      error_code ec;
      return make_address(remote.is_v4() ? "0.0.0.0" : "::", ec);
   }
   if (local->is_v4() != remote.is_v4())
      return std::nullopt;
   return local;
}

} // namespace anyhttp::io (in the runtime's namespace)
