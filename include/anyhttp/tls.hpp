#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ssl_st;
struct ssl_ctx_st;

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

/**
 * Makes \p ctx trust the CA certificates in the PEM file \p ca_file, or, if that is empty, the
 * default trust store of the TLS library. See \c client::Config::tls_ca_file. Throws a
 * \c system_error if the certificates cannot be loaded.
 */
void tls_load_trusted_cas(ssl_ctx_st* ctx, const std::string& ca_file);

/**
 * Makes \p ssl accept only a certificate valid for \p host, a name or an address. A name is sent as
 * SNI as well, which is for host names only. Returns whether that worked out.
 */
bool tls_set_peer_name(ssl_st* ssl, const std::string& host);

/**
 * Unpredictable bytes from the TLS library's own generator, for the `rand` callback that each of
 * nghttp2 version 2, nghttp3 and ngtcp2 asks for. Lives here because this is where the TLS library
 * is addressed; the signature is deliberately the one `nghttp2_rand` and `nghttp3_rand` declare,
 * so that it can be installed as the callback itself rather than wrapped.
 *
 * Does not return if the generator fails: AWS-LC's `RAND_bytes()` calls `abort()` itself, and this
 * does the same for OpenSSL's, which returns an error instead. Neither a return value nor an
 * exception would help a caller -- they are all C callbacks -- and predictable bytes would be worse
 * than stopping. A caller that *can* report the failure to its library still should, rather than
 * take the whole process down with it: see `cb_get_new_connection_id()`.
 */
void tls_random_bytes(uint8_t* dest, size_t destlen);

} // namespace anyhttp

// =================================================================================================
