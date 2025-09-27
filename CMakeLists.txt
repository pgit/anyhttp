cmake_minimum_required(VERSION 3.30)
project(asio_anyhttp VERSION 0.0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS On)
set(CMAKE_CXX_STANDARD 23)

if (DEFINED ENV{CI})
    add_compile_definitions(CI)
endif()

if (CMAKE_BUILD_TYPE MATCHES Debug)
    # https://www.boost.org/doc/libs/1_88_0/doc/html/boost_asio/overview/core/buffers.html#boost_asio.overview.core.buffers.buffer_debugging
    # add_compile_definitions(BOOST_ASIO_ENABLE_BUFFER_DEBUGGING)

    # ASIO handler tracking output (very noisy)
    # add_compile_definitions(BOOST_ASIO_ENABLE_HANDLER_TRACKING)
    
    # thread sanitizer
    # add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=thread)
    
    # address sanitizer
    # add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=address)

    # include(CodeCoverage)
    # append_coverage_compiler_flags()
    # SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
endif()
add_compile_options(-stdlib=libc++)
add_link_options(-stdlib=libc++ -lstdc++fs)

# use std::filesystem instead of boost::filesystem
add_compile_definitions(BOOST_PROCESS_USE_STD_FS)

# we don't use fmtlib any more
add_compile_definitions(SPDLOG_USE_STD_FORMAT)

# needed for ranges/v3 starting with clang 19
add_compile_options(-Wno-deprecated-declarations)

# FIXME: nghttp2 reader/writer needs this ATM
add_compile_options(-Wno-inconsistent-missing-override)

# --------------------------------------------------------------------------------------------------

#
# find boost
# https://github.com/madmongo1/blog-december-2020/blob/master/CMakeLists.txt
#
find_package(Boost 1.88 COMPONENTS thread atomic url filesystem process program_options OPTIONAL_COMPONENTS system REQUIRED)
find_package(OpenSSL)
find_package(Threads)

#
# nghttp2
# https://github.com/curl/curl/blob/master/CMake/FindNGHTTP2.cmake
#
# include_directories(${CMAKE_SOURCE_DIR}/../nghttp2/install/include)
# link_directories(${CMAKE_SOURCE_DIR}/../nghttp2/install/lib)
#
# or, just rely on pkg-config:
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

#
# range-v3 -- FIXME: needed only for views:chunked(),
#                    as the libc++ version doesn't seem to be available, yet
#
include_directories(/usr/local/include/range-v3)

#
# spdlog
#
find_package(spdlog REQUIRED)

# --------------------------------------------------------------------------------------------------

# Checks for header files, used by ngtcp2 code.
include(CheckIncludeFile)
check_include_file("arpa/inet.h"   HAVE_ARPA_INET_H)
check_include_file("netinet/in.h"  HAVE_NETINET_IN_H)
check_include_file("netinet/ip.h"  HAVE_NETINET_IP_H)
check_include_file("unistd.h"      HAVE_UNISTD_H)
check_include_file("sys/endian.h"  HAVE_SYS_ENDIAN_H)
check_include_file("endian.h"      HAVE_ENDIAN_H)
check_include_file("byteswap.h"    HAVE_BYTESWAP_H)
check_include_file("asm/types.h"   HAVE_ASM_TYPES_H)
check_include_file("linux/netlink.h"   HAVE_LINUX_NETLINK_H)
check_include_file("linux/rtnetlink.h" HAVE_LINUX_RTNETLINK_H)

# --------------------------------------------------------------------------------------------------

add_subdirectory(src)
add_subdirectory(test)
enable_testing()
