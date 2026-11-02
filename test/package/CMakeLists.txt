#
# A project outside of anyhttp that uses it the two ways a project can, see
# scripts/check_package.sh. It is not part of anyhttp's own build.
#
#   -DCMAKE_PREFIX_PATH=<prefix>                       an installed anyhttp, by find_package()
#   -DANYHTTP_EMBED=ON -DFETCHCONTENT_SOURCE_DIR_ANYHTTP=<source>   built along, by FetchContent
#
cmake_minimum_required(VERSION 3.30)
project(anyhttp_consumer LANGUAGES CXX)

option(ANYHTTP_EMBED "Build anyhttp along instead of using an installed one" OFF)

if (ANYHTTP_EMBED)
    include(FetchContent)
    # SYSTEM, so that the warnings of this project do not apply to anyhttp's headers
    FetchContent_Declare(anyhttp
        GIT_REPOSITORY https://github.com/pgit/anyhttp.git
        GIT_TAG master
        SYSTEM)
    FetchContent_MakeAvailable(anyhttp)
    foreach (target server client test_all pki)
        if (TARGET ${target})
            message(FATAL_ERROR "anyhttp as a subproject should not define the target '${target}'")
        endif()
    endforeach()
else()
    find_package(anyhttp 0.0 REQUIRED)
    message(STATUS "anyhttp ${anyhttp_VERSION}: ${anyhttp_API}, ${anyhttp_TLS_LIBRARY}, "
                   "${anyhttp_H2_LIBRARY} in ${anyhttp_DIR}")

    #
    # Every installed header has to compile on its own, with no more than the target gives: that
    # is what shows that nothing installed reaches for something that was left out.
    #
    get_target_property(include_dirs anyhttp::anyhttp INTERFACE_INCLUDE_DIRECTORIES)
    list(GET include_dirs 0 include_dir)
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE ${include_dir} ${include_dir}/anyhttp/*.hpp)
    foreach (header IN LISTS headers)
        string(MAKE_C_IDENTIFIER ${header} name)
        file(CONFIGURE OUTPUT headers/${name}.cpp CONTENT "#include <${header}>\n")
        list(APPEND header_tests ${CMAKE_CURRENT_BINARY_DIR}/headers/${name}.cpp)
    endforeach()
    add_library(header_tests OBJECT ${header_tests})
    target_link_libraries(header_tests PRIVATE anyhttp::anyhttp)
endif()

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE anyhttp::anyhttp)
target_compile_options(consumer PRIVATE -Wall -Wextra -Werror)
