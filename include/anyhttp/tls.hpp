#pragma once

#include <string>

struct ssl_st;

// =================================================================================================

namespace anyhttp
{

/**
 * One-line summary of a completed TLS handshake, in the spirit of what h2load prints:
 * protocol version, cipher, key exchange group and the negotiated ALPN protocol.
 *
 * Used for both TLS over TCP and QUIC, so that the log looks the same for all protocols.
 */
std::string tls_handshake_info(ssl_st* ssl);

} // namespace anyhttp

// =================================================================================================
