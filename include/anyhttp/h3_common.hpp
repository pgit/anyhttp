#pragma once

#include "anyhttp/common.hpp"

#include <nghttp3/nghttp3.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

//
// Shared HTTP/3 building blocks. Everything in this namespace is used by both roles: the server
// (src/h3_server.cpp) and the client (src/h3_client.cpp) differ only in the direction
// their messages travel, not in how a QUIC connection or an HTTP/3 stream is driven.
//
namespace anyhttp::http3
{

// =================================================================================================

/// Length of the connection IDs we mint for ourselves; also what the server's demux decodes with.
constexpr size_t QUIC_SCIDLEN = 18;

//
// Bound on how much of the caller's async_write() buffer a stream in WriteMode::Staged copies at
// a time -- copying is paced by how much nghttp3/ngtcp2 actually drains, rather than copying a
// huge caller buffer (e.g. 50MB) in one synchronous allocation+memcpy, mirroring nghttp2's own
// per-call copy into its frame buffer.
//
inline constexpr size_t kWriteChunkSize = 16 * 1024;

// =================================================================================================

/// Builds a nghttp3 name/value pair referencing (not copying) both strings.
nghttp3_nv make_nv(std::string_view name, std::string_view value);

/// Logs a block of headers, one per line, in the same style as the received ones.
void log_headers(std::string_view log_prefix, std::span<const nghttp3_nv> nva);

/// Same, for a header block buffered up by the recv_header callback.
void log_headers(std::string_view log_prefix,
                 const std::vector<std::pair<std::string, std::string>>& headers);

/// Installed as ngtcp2_settings::log_printf, but only when trace logging is enabled -- ngtcp2
/// formats every frame of every packet before calling it, so a callback that discards its input
/// still pays for the formatting, while a NULL one makes ngtcp2 skip that work entirely.
void ngtcp2_log_printf(void* user, const char* fmt, ...) noexcept;

// =================================================================================================

} // namespace anyhttp::http3
