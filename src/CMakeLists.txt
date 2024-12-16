cmake_minimum_required(VERSION 3.17)
project(anyhttp VERSION 0.0.1)

include_directories(${CMAKE_SOURCE_DIR}/include)

#
# lib
#
add_library(anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS *.cpp)
list(FILTER anyhttp_sources EXCLUDE REGEX "_main\.cpp$")
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_include_directories(anyhttp PUBLIC ../include)

target_include_directories(anyhttp PRIVATE "/opt/nghttp3/build/include")
target_sources(anyhttp PRIVATE ngtcp2/shared.cc)

target_link_libraries(anyhttp Threads::Threads)
target_link_libraries(anyhttp OpenSSL::SSL)
target_link_libraries(anyhttp Boost::system Boost::thread Boost::atomic Boost::coroutine Boost::url Boost::filesystem)
target_link_libraries(anyhttp nghttp2)
target_link_libraries(anyhttp fmt::fmt)
target_link_libraries(anyhttp spdlog::spdlog_header_only)

#
# server
#
add_executable(server)
target_sources(server PRIVATE "server_main.cpp")
target_link_libraries(server PRIVATE anyhttp)

#
# client
#
add_executable(client)
target_sources(client PRIVATE "client_main.cpp")
target_link_libraries(client PRIVATE anyhttp)

#
# ngtcp-test
#
add_executable(ngtcp-test)
target_sources(ngtcp-test PRIVATE "ngtcp-test_main.cpp")
target_include_directories(ngtcp-test PUBLIC "/opt/boringssl/include")
target_link_directories(ngtcp-test PRIVATE "/opt/boringssl/build/ssl")
target_link_directories(ngtcp-test PRIVATE "/opt/boringssl/build/crypto")
target_link_directories(ngtcp-test PRIVATE "/usr/local/lib")
target_link_libraries(ngtcp-test ssl crypto ev ngtcp2 libngtcp2_crypto_boringssl.a)

add_subdirectory(ngtcp2)
