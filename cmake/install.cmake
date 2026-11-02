#
# 'cmake --install', in two components:
#
#   Runtime      the programs as anyhttp-server and anyhttp-client, in a relocatable tree: everything
#                but the C runtime that they link dynamically is copied next to them.
#   Development  the library, its public headers and a CMake package, for find_package(anyhttp).
#
# The Development component is ASIO only: corosio writes install rules only when it is the top-level
# project, so a target that links it can't be exported. COROSIO is for FetchContent, which needs none
# of this, and its programs install all the same, as corosio links statically.
#
include(CMakePackageConfigHelpers)

# --------------------------------------------------------------------------------------------------

if (ANYHTTP_BUILD_APPS)
    set(anyhttp_programs server)
    if (ANYHTTP_ASIO)
        list(APPEND anyhttp_programs client)
    endif()

    set_target_properties(${anyhttp_programs} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS ${anyhttp_programs}
        RUNTIME_DEPENDENCY_SET anyhttp_runtime
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)

    # 'server' and 'client' are fine in a build tree, but not in /usr/local/bin.
    foreach (program IN LISTS anyhttp_programs)
        install(CODE "
            set(bin \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
            file(RENAME \"\${bin}/${program}\" \"\${bin}/anyhttp-${program}\")
            " COMPONENT Runtime)
    endforeach()

    # The C runtime is what stays behind, being tied to the kernel and the dynamic loader: a target
    # machine needs a glibc at least as new as the one built against.
    set(anyhttp_c_runtime
        "^(ld-linux|linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libgcc_s)[.-]")
    install(RUNTIME_DEPENDENCY_SET anyhttp_runtime
        PRE_EXCLUDE_REGEXES ${anyhttp_c_runtime}
        POST_EXCLUDE_REGEXES ".*/(ld-linux|libc|libm|libdl|libpthread|librt|libresolv|libgcc_s)[.-][^/]*$"
        DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime)
endif()

install(FILES ${PROJECT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/anyhttp COMPONENT Runtime)

# --------------------------------------------------------------------------------------------------

if (ANYHTTP_COROSIO)
    return()
endif()

set(anyhttp_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/anyhttp)

install(TARGETS anyhttp
    EXPORT anyhttp-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

#
# The public headers only. The protocol backends and the socket layer behind net.hpp stay out, which
# is what keeps nghttp2, ngtcp2 and nghttp3 from being compile-time dependencies of anyone who uses
# the library. scripts/check_package.sh compiles each header on its own against the installed tree,
# so one of them including something left out here fails there.
#
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/anyhttp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Development
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "corosio" EXCLUDE
    PATTERN "detail" EXCLUDE
    PATTERN "h1" EXCLUDE
    PATTERN "h2" EXCLUDE
    PATTERN "h2v2" EXCLUDE
    PATTERN "h3" EXCLUDE
    PATTERN "quic" EXCLUDE
    PATTERN "net.hpp" EXCLUDE
    PATTERN "*_impl.hpp" EXCLUDE
    PATTERN "any_async_stream.hpp" EXCLUDE
    PATTERN "buffer_array.hpp" EXCLUDE)
install(FILES ${PROJECT_BINARY_DIR}/include/anyhttp/config.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/anyhttp COMPONENT Development)

install(EXPORT anyhttp-targets
    NAMESPACE anyhttp::
    DESTINATION ${anyhttp_cmake_dir} COMPONENT Development)
configure_package_config_file(
    cmake/anyhttp-config.cmake.in
    ${PROJECT_BINARY_DIR}/anyhttp-config.cmake
    INSTALL_DESTINATION ${anyhttp_cmake_dir})
# anyhttp is pre-1.0, where a new minor version may break the interface
write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/anyhttp-config-version.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES
    ${PROJECT_BINARY_DIR}/anyhttp-config.cmake
    ${PROJECT_BINARY_DIR}/anyhttp-config-version.cmake
    DESTINATION ${anyhttp_cmake_dir} COMPONENT Development)
