#
# Boost.HTTP (https://github.com/cppalliance/http), the HTTP/1.1 parser and serializer of h1/ and
# the header field container that is anyhttp's Fields. Not part of a Boost release yet, so it comes
# like capy and corosio, by FetchContent at a pinned commit of 'develop'.
#
# Only its protocol core is built, as a target of our own, anyhttp_http (anyhttp::http when
# installed). Upstream's CMakeLists.txt builds all of src/ -- the router, the server, bcrypt and
# JSON -- and asks for Boost.JSON, which nothing here needs; SOURCE_SUBDIR keeps
# FetchContent_MakeAvailable() from adding it. The compression codecs are left out as well: the
# parser and serializer look them up as services at run time and do without them unless asked to
# (de)compress, which anyhttp never does. Their error categories are part of the core all the same.
#
# The patches in cmake/patches/ are proposed upstream. Each one says what it is for.
#
#   http-release-buffered-data.patch    parser::release_buffered_data(), a stub on 'develop': the
#                                       h2c upgrade hands what follows the request to HTTP/2
#   http-reserve-bytes-alignment.patch  fields_base::reserve_bytes() took any capacity, which
#                                       misaligns the table of fields at the end of the buffer
#                                       (UBSAN: "reference binding to misaligned address")
#
set(anyhttp_http_patches
    ${CMAKE_CURRENT_LIST_DIR}/patches/http-release-buffered-data.patch
    ${CMAKE_CURRENT_LIST_DIR}/patches/http-reserve-bytes-alignment.patch)

#
# The patch step runs when the source is fetched, and again whenever its command changes -- not when
# a patch file does, so each command carries the hash of its patch. Every run starts from the
# pristine files: a patch that has changed does not apply over its earlier version.
#
set(anyhttp_http_patch_command COMMAND git checkout --quiet -- .)
foreach (patch IN LISTS anyhttp_http_patches)
    file(SHA256 ${patch} anyhttp_http_patch_hash)
    list(APPEND anyhttp_http_patch_command COMMAND sh -c
        "git apply '${patch}' # ${anyhttp_http_patch_hash}")
endforeach()
unset(anyhttp_http_patch_hash)
list(POP_FRONT anyhttp_http_patch_command) # the first COMMAND is PATCH_COMMAND itself

FetchContent_Declare(http
    GIT_REPOSITORY https://github.com/cppalliance/http.git
    GIT_TAG 1f58e9791afd6c7351805f3074d4d3bbff8189f9
    PATCH_COMMAND ${anyhttp_http_patch_command}
    SOURCE_SUBDIR do-not-add-upstream-cmake
    EXCLUDE_FROM_ALL)

function(anyhttp_add_http)
    set(dir ${http_SOURCE_DIR})
    file(GLOB sources
        ${dir}/src/*.cpp
        ${dir}/src/core/*.cpp
        ${dir}/src/detail/*.cpp
        ${dir}/src/rfc/*.cpp
        ${dir}/src/rfc/detail/*.cpp
        ${dir}/src/brotli/error.cpp
        ${dir}/src/zlib/error.cpp
        ${dir}/src/zstd/error.cpp)
    list(FILTER sources EXCLUDE REGEX "/src/(application|datastore)\\.cpp$|/file_win32\\.cpp$")

    add_library(anyhttp_http STATIC ${sources})
    add_library(anyhttp::http ALIAS anyhttp_http)
    set_target_properties(anyhttp_http PROPERTIES EXPORT_NAME http)
    target_compile_features(anyhttp_http PUBLIC cxx_std_20)
    target_include_directories(anyhttp_http SYSTEM PUBLIC
        $<BUILD_INTERFACE:${dir}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(anyhttp_http PRIVATE ${dir})
    target_compile_definitions(anyhttp_http PUBLIC BOOST_HTTP_STATIC_LINK PRIVATE BOOST_HTTP_SOURCE)
    target_link_libraries(anyhttp_http PUBLIC Boost::capy Boost::headers Boost::url)
endfunction()
