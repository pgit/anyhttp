cmake_minimum_required(VERSION 3.30)
project(asio_anyhttp VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_EXPORT_COMPILE_COMMANDS On)

option(ENABLE_COVERAGE "Enable coverage flags and llvm-cov target" OFF)

if (DEFINED ENV{GITHUB_ACTIONS})
    add_compile_definitions(GITHUB_ACTIONS)
endif()

# --------------------------------------------------------------------------------------------------

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
    # source-based coverage (-fprofile-instr-generate, llvm-cov) exists in clang only
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE is supported only with Clang")
    endif()
    set(COVERAGE_RAW_DIR "${CMAKE_BINARY_DIR}/coverage_raw")
    add_compile_options(-O0 -g -fprofile-instr-generate=${COVERAGE_RAW_DIR}/%p.profraw -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate=${COVERAGE_RAW_DIR}/%p.profraw)
endif()

#
# Standard library: libc++ with clang, libstdc++ with GCC. The C++ libraries we link (Boost, fmt,
# spdlog) have to be built against the same one: the devcontainer has them for libstdc++ in
# /usr/local and for libc++ in /opt/libc++. GTest is built from source (see test/CMakeLists.txt).
#
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(USE_LIBCXX_DEFAULT ON)
else()
    set(USE_LIBCXX_DEFAULT OFF)
endif()
option(USE_LIBCXX "Build against libc++ instead of libstdc++" ${USE_LIBCXX_DEFAULT})

if (USE_LIBCXX)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
    set(LIBCXX_ROOT /opt/libc++ CACHE PATH "Installation prefix of C++ libraries built against libc++")
    list(PREPEND CMAKE_PREFIX_PATH ${LIBCXX_ROOT})
    # Statically: the shared libraries share their names with the libstdc++ ones in /usr/local/lib,
    # so an LD_LIBRARY_PATH pointing there would take precedence over our RUNPATH.
    set(Boost_USE_STATIC_LIBS ON)
    message(STATUS "Using C++ standard library: libc++ (libraries from ${LIBCXX_ROOT})")
else()
    message(STATUS "Using C++ standard library: libstdc++")
endif()

# --------------------------------------------------------------------------------------------------

# use std::filesystem instead of boost::filesystem
add_compile_definitions(BOOST_PROCESS_USE_STD_FS)

# Boost.Process v2's posix launcher calls asio's execution_context::notify_fork() in the forked
# child, between fork() and execve(). That walks the io_context's services and locks their
# mutexes -- and with more than one thread running the context, another thread may well have
# been holding one of them at the moment of the fork, so the child inherits it locked and hangs
# there forever, before it ever gets to dup2() its stdio or exec. The child does nothing but
# exec, so it has no use for the notification in the first place.
add_compile_definitions(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)

# don't allow use of deprecated ASIO interfaces
add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

#
# find boost
# https://github.com/madmongo1/blog-december-2020/blob/master/CMakeLists.txt
#
find_package(Boost 1.88 COMPONENTS thread atomic url filesystem process program_options REQUIRED)
find_package(Threads)

# --------------------------------------------------------------------------------------------------

#
# TLS: AWS-LC (default) or OpenSSL, selected with TLS_LIBRARY.
#
# AWS-LC is API compatible with BoringSSL, which is what ngtcp2 calls it; the devcontainer builds it
# into /opt/boringssl, deliberately away from the system OpenSSL. It only comes as static libraries.
# OpenSSL needs to be 3.5 or later, for the QUIC TLS API that ngtcp2_crypto_ossl builds on.
#
# Everything in the process has to use the very same TLS library: Boost.Asio's SSL streams,
# ngtcp2's crypto backend and our own code. Linking the other one on top would silently mix up two
# libraries exporting the same symbols. The code selects its variant from the headers it sees.
#
set(TLS_LIBRARY AWS-LC CACHE STRING "TLS library: AWS-LC or OpenSSL")
set_property(CACHE TLS_LIBRARY PROPERTY STRINGS AWS-LC OpenSSL)
if (TLS_LIBRARY STREQUAL "AWS-LC")
    set(BORINGSSL_ROOT /opt/boringssl CACHE PATH "Installation prefix of BoringSSL / AWS-LC")
    list(PREPEND CMAKE_PREFIX_PATH ${BORINGSSL_ROOT}) # ssl-config.cmake finds 'crypto' through it
    find_package(ssl CONFIG REQUIRED)
    set(TLS_TARGET AWS::ssl)
    set(NGTCP2_CRYPTO_MODULE libngtcp2_crypto_boringssl)
    message(STATUS "Using TLS library: AWS-LC (${ssl_DIR})")
elseif (TLS_LIBRARY STREQUAL "OpenSSL")
    find_package(OpenSSL 3.5 REQUIRED)
    set(TLS_TARGET OpenSSL::SSL)
    set(NGTCP2_CRYPTO_MODULE libngtcp2_crypto_ossl)
    message(STATUS "Using TLS library: OpenSSL ${OPENSSL_VERSION} (${OPENSSL_SSL_LIBRARY})")
else()
    message(FATAL_ERROR "TLS_LIBRARY must be AWS-LC or OpenSSL, not '${TLS_LIBRARY}'")
endif()

# --------------------------------------------------------------------------------------------------

#
# API style and I/O runtime, one per build tree: ASIO (completion tokens, on Boost.Asio) or COROSIO
# (coroutines only, on capy/corosio). The code tests ANYHTTP_COROSIO. See docs/corosio-port-plan.md.
#
set(ANYHTTP_API ASIO CACHE STRING "API style and I/O runtime: ASIO or COROSIO")
set_property(CACHE ANYHTTP_API PROPERTY STRINGS ASIO COROSIO)
if (ANYHTTP_API STREQUAL "ASIO")
    set(ANYHTTP_COROSIO 0)
elseif (ANYHTTP_API STREQUAL "COROSIO")
    set(ANYHTTP_COROSIO 1)

    # corosio's openssl_stream uses the zero-copy BIO pair API (BIO_nwrite0), which AWS-LC lacks
    if (NOT TLS_LIBRARY STREQUAL "OpenSSL")
        message(FATAL_ERROR "ANYHTTP_API=COROSIO needs TLS_LIBRARY=OpenSSL: "
                            "corosio's openssl_stream does not build against AWS-LC")
    endif()

    #
    # Pinned: both are pre-release and change their API on 'develop' without notice. Moving a pin
    # is a commit of its own.
    #
    include(FetchContent)
    foreach (option TESTS EXAMPLES BENCH)
        set(BOOST_CAPY_BUILD_${option} OFF CACHE BOOL "" FORCE)
        set(BOOST_COROSIO_BUILD_${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(capy
        GIT_REPOSITORY https://github.com/cppalliance/capy.git
        GIT_TAG a372a6b054261f29497ac0d19c2a9533f9eaad40
        EXCLUDE_FROM_ALL)
    FetchContent_Declare(corosio
        GIT_REPOSITORY https://github.com/cppalliance/corosio.git
        GIT_TAG a80ce002fd8cb27c893e50146990dc66d96cac02
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(capy corosio)
    message(STATUS "Using API style: COROSIO (capy/corosio)")
else()
    message(FATAL_ERROR "ANYHTTP_API must be ASIO or COROSIO, not '${ANYHTTP_API}'")
endif()
add_compile_definitions(ANYHTTP_COROSIO=${ANYHTTP_COROSIO})

# --------------------------------------------------------------------------------------------------

# FIXME: nghttp2 reader/writer needs this ATM
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-inconsistent-missing-override)
endif()

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

# --------------------------------------------------------------------------------------------------

# needed for ranges/v3 starting with clang 19
add_compile_options(-Wno-deprecated-declarations)


#
# range-v3 -- FIXME: needed only for views:chunked(),
#                    as the libc++ version doesn't seem to be available, yet
#
include_directories(/usr/local/include/range-v3)

# --------------------------------------------------------------------------------------------------

# we don't use fmtlib any more
add_compile_definitions(SPDLOG_USE_STD_FORMAT)

#
# spdlog
#
find_package(spdlog REQUIRED)

# --------------------------------------------------------------------------------------------------

enable_testing(false)
include(cmake/pki.cmake)
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
    set(LLVM_COV_IGNORE_REGEX "/usr/|${CMAKE_BINARY_DIR}/|${CMAKE_SOURCE_DIR}/test/")

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

# --------------------------------------------------------------------------------------------------
