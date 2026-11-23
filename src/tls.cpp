#include <anyhttp/tls.hpp>

#include "anyhttp/logging.hpp"
#include "anyhttp/net.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cerrno>
#include <cstdlib>
#include <format>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

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

// -------------------------------------------------------------------------------------------------

/// The key log file of an SSL_CTX, kept in its ex_data and closed when the context goes.
struct KeyLog
{
   explicit KeyLog(int fd) noexcept : fd(fd) {}
   ~KeyLog() { ::close(fd); }
   KeyLog(const KeyLog&) = delete;
   KeyLog& operator=(const KeyLog&) = delete;

   const int fd;
};

void free_keylog(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*)
{
   delete static_cast<KeyLog*>(ptr);
}

/// The ex_data slot every SSL_CTX keeps its KeyLog in, allocated on first use.
int keylog_index()
{
   static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, free_keylog);
   return index;
}

//
// The keylog callback has no argument of its own, so the file comes from the context. A line is
// written in one piece, newline and all: with O_APPEND, that keeps lines from different threads
// and processes apart. A write that fails loses that line, and only that line -- there is nobody
// to tell but the log.
//
void write_keylog_line(const SSL* ssl, const char* line)
{
   const auto* log =
      static_cast<KeyLog*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), keylog_index()));
   if (!log)
      return;

   std::string buffer{line};
   buffer += '\n';
   for (std::string_view rest = buffer; !rest.empty();)
   {
      const auto n = ::write(log->fd, rest.data(), rest.size());
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
      {
         logw("TLS key log: {}", last_error().message());
         return;
      }
      rest.remove_prefix(static_cast<size_t>(n));
   }
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
// A system_error, as that is what Client::connect() turns into its result. What went wrong is too
// much for an error code, so that goes to the log.
//
void tls_load_trusted_cas(ssl_ctx_st* ctx, const std::string& ca_file)
{
   const bool loaded = ca_file.empty()
                          ? SSL_CTX_set_default_verify_paths(ctx) == 1
                          : SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) == 1;
   if (!loaded)
   {
      loge("can't load CA certificates from '{}': {}", ca_file,
           ERR_error_string(ERR_get_error(), nullptr));
      throw_error(make_error_code(errc::invalid_argument));
   }
}

// -------------------------------------------------------------------------------------------------

//
// An IP address is matched against the certificate's IP SANs, and not sent as SNI.
//
bool tls_set_peer_name(ssl_st* ssl, const std::string& host)
{
   auto* param = SSL_get0_param(ssl);
   error_code not_an_ip;
   io::make_address(host, not_an_ip);
   return !not_an_ip ? X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) == 1
                     : X509_VERIFY_PARAM_set1_host(param, host.data(), host.size()) == 1 &&
                          SSL_set_tlsext_host_name(ssl, host.c_str()) == 1;
}

// -------------------------------------------------------------------------------------------------

//
// AWS-LC and OpenSSL have the same keylog callback and the same ex_data, so this needs no #if
// either. Setting it again replaces the file, closing the one before.
//
void tls_set_keylog_file(ssl_ctx_st* ctx, const std::string& file)
{
   if (file.empty())
      return;

   const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
   if (fd < 0)
   {
      const auto ec = last_error();
      loge("can't open TLS key log '{}': {}", file, ec.message());
      throw_error(ec);
   }

   auto* log = new KeyLog(fd);
   delete static_cast<KeyLog*>(SSL_CTX_get_ex_data(ctx, keylog_index()));
   SSL_CTX_set_ex_data(ctx, keylog_index(), log);
   SSL_CTX_set_keylog_callback(ctx, write_keylog_line);
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
