#pragma once

#include "anyhttp/quic/common.hpp"

#include <nghttp3/nghttp3.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

//
// Shared HTTP/3 building blocks, used by both roles: the server (src/h3/server.cpp) and the client
// (src/h3/client.cpp) differ only in the direction their messages travel, not in how an HTTP/3
// stream is driven. What is about QUIC rather than HTTP/3 is in anyhttp/quic/common.hpp.
//
namespace anyhttp::http3
{

// =================================================================================================

/// Builds a nghttp3 name/value pair referencing (not copying) both strings.
nghttp3_nv make_nv(std::string_view name, std::string_view value);

/// Logs a block of headers, one per line, in the same style as the received ones.
void log_headers(std::string_view log_prefix, std::span<const nghttp3_nv> nva);

/// Same, for a header block buffered up by the recv_header callback.
void log_headers(std::string_view log_prefix,
                 const std::vector<std::pair<std::string, std::string>>& headers);

// =================================================================================================

} // namespace anyhttp::http3
