# nice sample:
# https://github.com/andrewmd5/libwtf/blob/main/CMakeLists.txt

#
# QUIC / HTTP3 libraries
#
pkg_check_modules(NGTCP2              REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(NGHTTP3             REQUIRED IMPORTED_TARGET libnghttp3)
pkg_check_modules(NGTCP2_CRYPTO          REQUIRED IMPORTED_TARGET ${NGTCP2_CRYPTO_MODULE})

#
# lib -- the protocol backends are in h1/, h2/ or h2v2/, quic/ and h3/; only h2/ uses nghttp2,
# only h2v2/ nghttp2 v2, only quic/ and h3/ ngtcp2, and only h3/ nghttp3. The two HTTP/2 backends
# are alternatives (see H2_LIBRARY), so only the one of this build tree is compiled -- same for
# asio/ and corosio/. Everything at the top level is protocol- and runtime-agnostic.
#
string(TOLOWER ${ANYHTTP_API} anyhttp_runtime)
if (ANYHTTP_H2_V2)
    set(anyhttp_h2 h2v2)
else()
    set(anyhttp_h2 h2)
endif()
add_library(anyhttp)
add_library(anyhttp::anyhttp ALIAS anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS
     *.cpp h1/*.cpp ${anyhttp_h2}/*.cpp h3/*.cpp quic/*.cpp ${anyhttp_runtime}/*.cpp)
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_compile_features(anyhttp PUBLIC cxx_std_23)
target_include_directories(anyhttp PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>)

#
# What the public headers need comes with the target, because whoever includes them has to see the
# same as the library did: Asio and spdlog are header-only, so their configuration is part of what
# everyone compiles, and two different standard libraries would not even link.
#
target_compile_definitions(anyhttp PUBLIC
    BOOST_ASIO_NO_DEPRECATED # don't allow use of deprecated ASIO interfaces
    SPDLOG_USE_STD_FORMAT)   # we don't use fmtlib any more
if (USE_LIBCXX)
    target_compile_options(anyhttp PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    target_link_options(anyhttp PUBLIC -stdlib=libc++)
endif()
target_link_libraries(anyhttp PUBLIC Threads::Threads Boost::headers Boost::url)
target_link_libraries(anyhttp PUBLIC spdlog::spdlog_header_only)

target_link_libraries(anyhttp PRIVATE Boost::thread Boost::atomic Boost::filesystem)
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

if (NOT ANYHTTP_BUILD_APPS)
    return()
endif()

#
# server
#
add_executable(server)
target_sources(server PRIVATE "apps/server_main.cpp")
target_link_libraries(server PRIVATE anyhttp)
target_link_libraries(server PRIVATE Boost::program_options)

#
# client
#
add_executable(client)
target_sources(client PRIVATE "apps/client_main.cpp")
target_link_libraries(client PRIVATE anyhttp)

if (ANYHTTP_COROSIO)
    #
    # corosio_issue_reactor_uaf: the reactor's use-after-free with several threads, standalone
    #
    add_executable(corosio_issue_reactor_uaf)
    target_sources(corosio_issue_reactor_uaf PRIVATE "issues/corosio_issue_reactor_uaf.cpp")
    target_link_libraries(corosio_issue_reactor_uaf PRIVATE Boost::corosio Threads::Threads)

    return()
endif()

#
# beast_issue_3032
#
add_executable(beast_issue_3032)
target_sources(beast_issue_3032 PRIVATE "issues/beast_issue_3032.cpp")
