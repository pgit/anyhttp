#include <anyhttp/tls.hpp>

#include "anyhttp/logging.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <cstdlib>
#include <format>

// =================================================================================================

namespace anyhttp
{

namespace
{

/**
 * Key exchange group of the handshake, like the "Server Temp Key" line of h2load, e.g.
 * "X25519 (253 bits)" or "P-256 (256 bits)" -- or "prime256v1 (256 bits)" with OpenSSL.
 */
std::string key_exchange(SSL* ssl)
{
   //
   // The negotiated group is known even for groups that have no EVP_PKEY, like the post-quantum
   // hybrids ("X25519MLKEM768").
   //
#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
   const char* group = SSL_get_group_name(SSL_get_group_id(ssl));
#else
   const char* group = SSL_get0_group_name(ssl);
#endif
   std::string name = group ? group : "unknown";

   //
   // On the client this is the server's key share, on the server the client's one. Either way,
   // both peers agree on the group, which is what we are after.
   //
   EVP_PKEY* key = nullptr;
   if (SSL_get_peer_tmp_key(ssl, &key) != 1 || !key)
      return name;

   auto bits = EVP_PKEY_bits(key);
   EVP_PKEY_free(key);

   return std::format("{} ({} bits)", name, bits);
}

std::string_view alpn(SSL* ssl)
{
   const unsigned char* data = nullptr;
   unsigned int len = 0;
   SSL_get0_alpn_selected(ssl, &data, &len);
   if (!data)
      return "none";
   return {reinterpret_cast<const char*>(data), len};
}

} // namespace

// -------------------------------------------------------------------------------------------------

std::string tls_handshake_info(ssl_st* ssl)
{
   if (!ssl)
      return "no TLS session";

   return std::format("{}, cipher={}, group={}, alpn={}{}", SSL_get_version(ssl),
                      SSL_get_cipher_name(ssl), key_exchange(ssl), alpn(ssl),
                      SSL_session_reused(ssl) ? ", resumed" : "");
}

// -------------------------------------------------------------------------------------------------

//
// Needs no variant #if, unlike everything above: AWS-LC and OpenSSL both have RAND_bytes.
//
// AWS-LC never returns from a failure -- "in the event that sufficient random data can not be
// obtained, |abort| is called" -- so the branch below is dead code there. OpenSSL does return, and
// there is nothing useful to do with that: every caller is a C callback of nghttp2, nghttp3 or
// ngtcp2, so an exception cannot pass, and continuing with predictable bytes would be worse than
// stopping. Abort, to behave the same way on both.
//
void tls_random_bytes(uint8_t* dest, size_t destlen)
{
   if (RAND_bytes(dest, static_cast<int>(destlen)) != 1)
   {
      loge("tls_random_bytes: RAND_bytes({}) failed, aborting", destlen);
      std::abort();
   }
}

} // namespace anyhttp

// =================================================================================================
