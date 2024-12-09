cmake_minimum_required(VERSION 3.17)
project(anyhttp VERSION 0.0.1)

add_executable(ngtcp-client)
target_sources(ngtcp-client PRIVATE
   client.cc
   client_base.cc
   debug.cc
   util.cc
   shared.cc
   tls_client_context_boringssl.cc
   tls_client_session_boringssl.cc
   tls_session_base_quictls.cc
   tls_shared_boringssl.cc
   util_openssl.cc
)
target_compile_definitions(ngtcp-client PRIVATE ENABLE_EXAMPLE_BORINGSSL WITH_EXAMPLE_BORINGSSL)
target_compile_definitions(ngtcp-client PRIVATE HAVE_NETINET_IN_H HAVE_ARPA_INET_H)

target_include_directories(ngtcp-client PRIVATE ".")
target_include_directories(ngtcp-client PRIVATE "/opt/boringssl/include")
target_include_directories(ngtcp-client PRIVATE "/opt/ngtcp2/third-party/urlparse")
target_include_directories(ngtcp-client PRIVATE "/opt/nghttp3/build/include")
target_link_directories(ngtcp-client PRIVATE "/opt/boringssl/build/ssl")
target_link_directories(ngtcp-client PRIVATE "/opt/boringssl/build/crypto")
target_link_directories(ngtcp-client PRIVATE "/opt/nghttp3/build/lib")
target_link_directories(ngtcp-client PRIVATE "/opt/ngtcp2/third-party/urlparse")
target_link_directories(ngtcp-client PRIVATE "/usr/local/lib")
target_link_libraries(ngtcp-client PRIVATE ssl crypto ev)
target_link_libraries(ngtcp-client PRIVATE ngtcp2 libngtcp2_crypto_boringssl.a)
target_link_libraries(ngtcp-client PRIVATE nghttp3)
target_link_libraries(ngtcp-client PUBLIC /opt/ngtcp2/third-party/urlparse/liburlparse_la-urlparse.o)
