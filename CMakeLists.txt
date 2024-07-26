cmake_minimum_required(VERSION 3.17)
project(asio_anyhttp VERSION 0.0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS On)
set(CMAKE_CXX_STANDARD 23)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(BOOST_ASIO_ENABLE_BUFFER_DEBUGGING)

    # ASIO handler tracking output (very noisy)
    # add_compile_definitions(BOOST_ASIO_ENABLE_HANDLER_TRACKING)
    
    # add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=thread)
    
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)

    # include(CodeCoverage)
    # append_coverage_compiler_flags()
    # SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
endif()
add_compile_options(-stdlib=libc++)
add_link_options(-stdlib=libc++ -lstdc++fs)

# --------------------------------------------------------------------------------------------------

#
# find boost
# https://github.com/madmongo1/blog-december-2020/blob/master/CMakeLists.txt
#
find_package(Boost COMPONENTS system thread atomic coroutine url filesystem REQUIRED)
if (Boost_VERSION VERSION_LESS 1.78)
    message(FATAL_ERROR "Boost 1.78 or higher required")
endif ()
find_package(OpenSSL COMPONENTS)
find_package(Threads)

#
# nghttp2
# https://github.com/curl/curl/blob/master/CMake/FindNGHTTP2.cmake
#
include_directories(${CMAKE_SOURCE_DIR}/../nghttp2/install/include)
link_directories(${CMAKE_SOURCE_DIR}/../nghttp2/install/lib)

#
# fmt
#
find_package(fmt)

#
# fmtlog
#
# include_directories(/fmtlog/fmt/include)
# link_directories(/usr/lib)

#
# range-v3
#
include_directories(/usr/local/include/range-v3)

#
# spodlog
#
find_package(spdlog REQUIRED)

# --------------------------------------------------------------------------------------------------

add_subdirectory(src)
add_subdirectory(test)
