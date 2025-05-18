cmake_minimum_required(VERSION 3.17)
project(anyhttp VERSION 0.0.1)

pkg_check_modules(NGHTTP3 REQUIRED IMPORTED_TARGET libnghttp3)
pkg_check_modules(NGTCP2 REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGTCP2_CRYPTO_BORINGSSL REQUIRED IMPORTED_TARGET libngtcp2_crypto_boringssl)

set(NGTCP2_COMMON_SOURCES
   debug.cc
   util.cc
   shared.cc
   tls_shared_boringssl.cc
   tls_session_base_quictls.cc
   util_openssl.cc
)

set(NGTCP2_CLIENT_SOURCES
   client.cc
   client_base.cc
   tls_client_context_boringssl.cc
   tls_client_session_boringssl.cc
)

set(NGTCP2_SERVER_SOURCES
   server.cc
   server_base.cc
   http.cc
   tls_server_context_boringssl.cc
   tls_server_session_boringssl.cc
)

foreach(name IN ITEMS ngtcp-client ngtcp-server)
    add_executable(${name})
    if(name STREQUAL "ngtcp-client")
       target_sources(${name} PRIVATE ${NGTCP2_CLIENT_SOURCES} ${NGTCP2_COMMON_SOURCES})
    else()
       target_sources(${name} PRIVATE ${NGTCP2_SERVER_SOURCES} ${NGTCP2_COMMON_SOURCES})
    endif()
    target_compile_definitions(${name} PRIVATE ENABLE_EXAMPLE_BORINGSSL WITH_EXAMPLE_BORINGSSL)
    target_compile_definitions(${name} PRIVATE HAVE_NETINET_IN_H HAVE_ARPA_INET_H)
    target_include_directories(${name} PRIVATE "." "/opt/boringssl/include")
    target_link_directories(${name} PRIVATE "/opt/boringssl/lib")
    target_link_libraries(${name} PRIVATE PkgConfig::NGHTTP3)
    target_link_libraries(${name} PRIVATE PkgConfig::NGTCP2)
    target_link_libraries(${name} PRIVATE PkgConfig::NGTCP2_CRYPTO_BORINGSSL)
    target_link_libraries(${name} PRIVATE liburlparse.a)
    target_link_libraries(${name} PRIVATE ssl crypto ev)
    target_link_libraries(${name} PRIVATE nghttp3)
endforeach()

