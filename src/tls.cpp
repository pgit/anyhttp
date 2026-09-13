#include <anyhttp/tls.hpp>

#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <format>

// =================================================================================================

namespace anyhttp
{

namespace
{

/**
 * Key exchange group of the handshake, like the "Server Temp Key" line of h2load, e.g.
 * "X25519 (253 bits)" or "P-256 (256 bits)".
 */
std::string key_exchange(SSL* ssl)
{
   //
   // The negotiated group is known even for groups that have no EVP_PKEY, like the post-quantum
   // hybrids ("X25519MLKEM768").
   //
   const char* group = SSL_get_group_name(SSL_get_group_id(ssl));
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

} // namespace anyhttp

// =================================================================================================
