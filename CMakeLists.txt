cmake_minimum_required(VERSION 3.30)
project(asio_anyhttp VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_EXPORT_COMPILE_COMMANDS On)

option(ENABLE_COVERAGE "Enable coverage flags and llvm-cov target" OFF)

if (DEFINED ENV{GITHUB_ACTIONS})
    add_compile_definitions(GITHUB_ACTIONS)
endif()

if (CMAKE_BUILD_TYPE MATCHES Debug)
    # https://www.boost.org/doc/libs/1_89_0/doc/html/boost_asio/overview/core/buffers.html#boost_asio.overview.core.buffers.buffer_debugging
    # add_compile_definitions(BOOST_ASIO_ENABLE_BUFFER_DEBUGGING)

    # ASIO handler tracking output (very noisy)
    # add_compile_definitions(BOOST_ASIO_ENABLE_HANDLER_TRACKING)

    # thread sanitizer
    # add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=thread)

    # address sanitizer
    # add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=address)
endif()

if (ENABLE_COVERAGE)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE is supported only with GNU or Clang compilers")
    endif()
    set(COVERAGE_RAW_DIR "${CMAKE_BINARY_DIR}/coverage_raw")
    add_compile_options(-O0 -g -fprofile-instr-generate=${COVERAGE_RAW_DIR}/%p.profraw -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate=${COVERAGE_RAW_DIR}/%p.profraw)
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

enable_testing(false)
add_subdirectory(src)
add_subdirectory(test)

# --------------------------------------------------------------------------------------------------

if (ENABLE_COVERAGE)
    find_program(LLVM_COV_EXECUTABLE llvm-cov)
    find_program(LLVM_PROFDATA_EXECUTABLE llvm-profdata)
    if (NOT LLVM_COV_EXECUTABLE OR NOT LLVM_PROFDATA_EXECUTABLE)
        message(FATAL_ERROR "llvm-cov and llvm-profdata are required for coverage reporting")
    endif()

    set(COVERAGE_RAW_DIR "${CMAKE_BINARY_DIR}/coverage_raw")
    set(COVERAGE_PROFDATA_FILE "${CMAKE_BINARY_DIR}/coverage.profdata")
    set(COVERAGE_HTML_DIR "${CMAKE_BINARY_DIR}/coverage_report")
    set(COVERAGE_MERGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/merge_profraw.cmake")
    set(COVERAGE_TEST_BIN "${CMAKE_BINARY_DIR}/test/test_all")
    set(LLVM_COV_IGNORE_REGEX "/usr/|${CMAKE_BINARY_DIR}/|${CMAKE_SOURCE_DIR}/test/|${CMAKE_SOURCE_DIR}/src/ngtcp2/")

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${COVERAGE_RAW_DIR} ${COVERAGE_HTML_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_RAW_DIR}
        COMMAND ${CMAKE_COMMAND} -E env
            LLVM_PROFILE_FILE=${COVERAGE_RAW_DIR}/%p.profraw
            ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND}
            -DPROFRAW_DIR=${COVERAGE_RAW_DIR}
            -DPROFDATA_FILE=${COVERAGE_PROFDATA_FILE}
            -DLLVM_PROFDATA_EXECUTABLE=${LLVM_PROFDATA_EXECUTABLE}
            -P ${COVERAGE_MERGE_SCRIPT}
        COMMAND ${LLVM_COV_EXECUTABLE} show
            ${COVERAGE_TEST_BIN}
            -instr-profile=${COVERAGE_PROFDATA_FILE}
            -format=html
            -output-dir=${COVERAGE_HTML_DIR}
            -ignore-filename-regex=${LLVM_COV_IGNORE_REGEX}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running tests and generating llvm-cov HTML report"
        VERBATIM
    )
    add_dependencies(coverage test_all)
endif()
