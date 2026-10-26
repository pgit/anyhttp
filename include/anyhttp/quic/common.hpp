#pragma once

#include "anyhttp/common.hpp"

#include <ngtcp2/ngtcp2.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <optional>
#include <string>
#include <string_view>

//
// Shared QUIC building blocks. Everything in this namespace is used by both roles -- the server
// (src/quic/server.cpp) and the client (src/quic/client.cpp) -- and by every application protocol
// running on top of a connection, HTTP/3 (src/h3/) as much as plain streams.
//
namespace anyhttp::quic
{

// =================================================================================================

/// Length of the connection IDs we mint for ourselves; also what the server's demux decodes with.
constexpr size_t QUIC_SCIDLEN = 18;

//
// Bound on how much of the caller's async_write() buffer a stream in WriteMode::Staged copies at
// a time -- copying is paced by how much ngtcp2 actually drains, rather than copying a huge
// caller buffer (e.g. 50MB) in one synchronous allocation+memcpy, mirroring nghttp2's own
// per-call copy into its frame buffer.
//
inline constexpr size_t kWriteChunkSize = 16 * 1024;

/// Installed as ngtcp2_settings::log_printf, but only when trace logging is enabled -- ngtcp2
/// formats every frame of every packet before calling it, so a callback that discards its input
/// still pays for the formatting, while a NULL one makes ngtcp2 skip that work entirely.
void ngtcp2_log_printf(void* user, const char* fmt, ...) noexcept;

// =================================================================================================
//
// Below: helpers taken from the ngtcp2 examples (https://github.com/ngtcp2/ngtcp2, examples/,
// v1.25.0, MIT licensed), reduced to what anyhttp actually uses. They used to be vendored
// verbatim under src/ngtcp2/.
//

/// A socket address of any of the families we speak, as ngtcp2 hands them around.
union sockaddr_union
{
   sockaddr_storage storage;
   sockaddr sa;
   sockaddr_in6 in6;
   sockaddr_in in;
};

/// A socket address together with its actual length and the interface it was seen on.
struct Address
{
   socklen_t len;
   union sockaddr_union su;
   uint32_t ifindex;
};

/// Returns the local (destination) address of the packet described by \p msg, as delivered by
/// IP(V6)_PKTINFO. \p family is the address family the packet was received from.
std::optional<Address> msghdr_get_local_addr(msghdr* msg, int family);

/// Copies the port of \p src into \p dst.
void set_port(Address& dst, const Address& src);

/// The current steady clock reading in nanoseconds, which is the timestamp ngtcp2 expects.
ngtcp2_tstamp timestamp();

/// The log prefix for a peer at \p sa of length \p salen, see anyhttp::log_prefix().
std::string log_prefix(Role role, std::string_view protocol, const sockaddr* sa, socklen_t salen);

/// Formats \p len bytes at \p data as lowercase hex, for logging connection IDs.
std::string format_hex(const uint8_t* data, size_t len);

// =================================================================================================

} // namespace anyhttp::quic
