cmake_minimum_required(VERSION 3.17)
project(anyhttp VERSION 0.0.1)

include_directories(${CMAKE_SOURCE_DIR}/include)

#
# lib
#
add_library(anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS *.cpp)
list(FILTER anyhttp_sources EXCLUDE REGEX "_main\.cpp$")
list(FILTER anyhttp_sources EXCLUDE REGEX "_issue_[0-9]+\.cpp$")
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_include_directories(anyhttp PUBLIC ../include)

target_include_directories(anyhttp PRIVATE "/opt/nghttp3/build/include")
target_sources(anyhttp PRIVATE ngtcp2/shared.cc)

target_link_libraries(anyhttp PRIVATE Threads::Threads)
target_link_libraries(anyhttp PRIVATE OpenSSL::SSL)
target_link_libraries(anyhttp PRIVATE Boost::thread Boost::atomic Boost::url Boost::filesystem)
target_link_libraries(anyhttp PRIVATE spdlog::spdlog_header_only)
target_link_libraries(anyhttp PRIVATE PkgConfig::NGHTTP2)

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

#
# ngtcp-test
#
add_executable(ngtcp-test)
target_sources(ngtcp-test PRIVATE "ngtcp-test_main.cpp")
target_include_directories(ngtcp-test PUBLIC "/opt/boringssl/include")
target_link_directories(ngtcp-test PRIVATE "/opt/boringssl/lib")
target_link_libraries(ngtcp-test ssl crypto ev ngtcp2 libngtcp2_crypto_boringssl.a)

add_subdirectory(ngtcp2)
