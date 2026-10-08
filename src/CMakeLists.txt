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
# lib -- the protocol backends are in h1/, h2/ and h3/; only h2/ uses nghttp2 and only h3/
# ngtcp2/nghttp3. What differs between the API styles is in asio/ or corosio/, and only the one of
# this build tree is compiled. Everything at the top level is protocol- and runtime-agnostic.
#
string(TOLOWER ${ANYHTTP_API} anyhttp_runtime)
add_library(anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS
     *.cpp h1/*.cpp h2/*.cpp h3/*.cpp ${anyhttp_runtime}/*.cpp)
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_include_directories(anyhttp PUBLIC ../include ${CMAKE_BINARY_DIR}/include)

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

if (ANYHTTP_COROSIO)
    target_link_libraries(anyhttp PUBLIC Boost::corosio Boost::corosio_openssl)
endif()

#
# server
#
add_executable(server)
target_sources(server PRIVATE "apps/server_main.cpp")
target_link_libraries(server PRIVATE anyhttp)
target_link_libraries(server PRIVATE Boost::program_options)

if (ANYHTTP_COROSIO)
    #
    # corosio_issue_reactor_uaf: the reactor's use-after-free with several threads, standalone
    #
    add_executable(corosio_issue_reactor_uaf)
    target_sources(corosio_issue_reactor_uaf PRIVATE "issues/corosio_issue_reactor_uaf.cpp")
    target_link_libraries(corosio_issue_reactor_uaf PRIVATE Boost::corosio Threads::Threads)

    # the client program comes with the rest of the port
    return()
endif()

#
# client
#
add_executable(client)
target_sources(client PRIVATE "apps/client_main.cpp")
target_link_libraries(client PRIVATE anyhttp)

#
# beast_issue_3032
#
add_executable(beast_issue_3032)
target_sources(beast_issue_3032 PRIVATE "issues/beast_issue_3032.cpp")
