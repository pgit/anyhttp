pkg_check_modules(NGHTTP3 REQUIRED IMPORTED_TARGET libnghttp3)
pkg_check_modules(NGTCP2  REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGTCP2_CRYPTO_OSSL      REQUIRED IMPORTED_TARGET libngtcp2_crypto_ossl)
pkg_check_modules(NGTCP2_CRYPTO_BORINGSSL REQUIRED IMPORTED_TARGET libngtcp2_crypto_boringssl)

#
# Create a static library from supplementary ngtcp2/example code.
# The following definitions apply to both the lib and the example programs below.
#
# add_compile_definitions(ENABLE_EXAMPLE_BORINGSSL WITH_EXAMPLE_BORINGSSL)
add_compile_definitions(ENABLE_EXAMPLE_OSSL WITH_EXAMPLE_OSSL)
add_compile_definitions(HAVE_NETINET_UDP_H)
add_compile_definitions(HAVE_NETINET_IN_H)
add_compile_definitions(HAVE_ARPA_INET_H)
# include_directories("." "/opt/boringssl/include")
# link_directories("/opt/boringssl/lib")

add_library(ngtcp2_common STATIC)
target_sources(ngtcp2_common PRIVATE
   debug.cc
   util.cc
   shared.cc
   tls_shared_boringssl.cc
   tls_session_base_ossl.cc
   util_openssl.cc
   siphash.cc
)
target_link_libraries(ngtcp2_common PRIVATE
   PkgConfig::NGTCP2
)

#
# Create example client and server binaries. The following link_libraries() directives apply only
# to those.
#
link_libraries(ngtcp2_common)
link_libraries(PkgConfig::NGHTTP3 PkgConfig::NGTCP2 PkgConfig::NGTCP2_CRYPTO_OSSL)
link_libraries(liburlparse.a ssl crypto ev nghttp3)

add_executable(ngtcp-client)
target_sources(ngtcp-client PRIVATE
   client.cc
   client_base.cc
   tls_client_context_ossl.cc
   tls_client_session_ossl.cc
)

add_executable(ngtcp-server)
target_sources(ngtcp-server PRIVATE
   server.cc
   server_base.cc
   http.cc
   tls_server_context_ossl.cc
   tls_server_session_ossl.cc
)
