include_directories(${CMAKE_SOURCE_DIR}/include)

# nice sample:
# https://github.com/andrewmd5/libwtf/blob/main/CMakeLists.txt

#
# QUIC / HTTP3 libraries
#
pkg_check_modules(NGTCP2              REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGHTTP3             REQUIRED IMPORTED_TARGET libnghttp3)
pkg_check_modules(NGTCP2_CRYPTO          REQUIRED IMPORTED_TARGET ${NGTCP2_CRYPTO_MODULE})

#
# lib -- the protocol backends are the h1_*, h2_* and h3_* sources; only the h2_* ones use
# nghttp2 and only the h3_* ones ngtcp2/nghttp3, everything else is protocol-agnostic.
#
add_library(anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS *.cpp)
list(FILTER anyhttp_sources EXCLUDE REGEX "_main\.cpp$")
list(FILTER anyhttp_sources EXCLUDE REGEX "_issue_[0-9]+\.cpp$")
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_include_directories(anyhttp PUBLIC ../include)

target_include_directories(anyhttp PRIVATE "/opt/nghttp3/build/include")
target_link_libraries(anyhttp PRIVATE Threads::Threads)
target_link_libraries(anyhttp PRIVATE Boost::thread Boost::atomic Boost::url Boost::filesystem)
target_link_libraries(anyhttp PRIVATE spdlog::spdlog_header_only)
target_link_libraries(anyhttp PRIVATE PkgConfig::NGHTTP2)
target_link_libraries(anyhttp PRIVATE PkgConfig::NGTCP2_CRYPTO PkgConfig::NGTCP2 PkgConfig::NGHTTP3)

# PUBLIC, because the backend headers include Boost.Asio's SSL streams: whoever includes them must
# see the headers of the selected TLS library -- with AWS-LC, not the system OpenSSL ones in
# /usr/include. AWS-LC comes as static libraries, so this comes after ngtcp2's crypto backend,
# which needs it.
target_link_libraries(anyhttp PUBLIC ${TLS_TARGET})

#
# server
#
add_executable(server)
target_sources(server PRIVATE "server_main.cpp")
target_link_libraries(server PRIVATE anyhttp)
target_link_libraries(server PRIVATE Boost::program_options)

#
# client
#
add_executable(client)
target_sources(client PRIVATE "client_main.cpp")
target_link_libraries(client PRIVATE anyhttp)

#
# beast_issue_3032
#
add_executable(beast_issue_3032)
target_sources(beast_issue_3032 PRIVATE "beast_issue_3032.cpp")
