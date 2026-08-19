#
# Test PKI generation.
#
# The server and the tests open their certificates through paths relative to the project root
# (see src/server_impl.cpp and test/test_server.cpp), so the PKI has to be generated into the
# source tree at pki/out rather than into the build directory.
#
# Nothing below pki/out is tracked by git: the whole chain is generated here at build time.
#
find_program(CFSSL_EXECUTABLE cfssl REQUIRED)
find_program(CFSSLJSON_EXECUTABLE cfssljson REQUIRED)

set(PKI_DIR "${CMAKE_SOURCE_DIR}/pki")
set(PKI_OUT_DIR "${PKI_DIR}/out")

set(PKI_INPUTS
    "${PKI_DIR}/create.sh"
    "${PKI_DIR}/config.json"
    "${PKI_DIR}/root.json"
    "${PKI_DIR}/intermediate.json"
    "${PKI_DIR}/server.json"
    "${PKI_DIR}/client.json"
)

set(PKI_OUTPUTS
    "${PKI_OUT_DIR}/root.pem"
    "${PKI_OUT_DIR}/intermediate.pem"
    "${PKI_OUT_DIR}/server.pem"
    "${PKI_OUT_DIR}/server-key.pem"
    "${PKI_OUT_DIR}/server-chain.pem"
    "${PKI_OUT_DIR}/client.pem"
    "${PKI_OUT_DIR}/client-key.pem"
)

#
# create.sh skips any stage whose output already exists, which would leave a stale leaf signed by
# a CA that is no longer around after one of the .json profiles changes. Wipe pki/out first so a
# regeneration always produces one self-consistent chain.
#
add_custom_command(
    OUTPUT ${PKI_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PKI_OUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PKI_OUT_DIR}"
    COMMAND "${PKI_DIR}/create.sh"
    WORKING_DIRECTORY "${PKI_DIR}"
    DEPENDS ${PKI_INPUTS}
    COMMENT "Generating test PKI in pki/out"
    VERBATIM
)

add_custom_target(pki ALL DEPENDS ${PKI_OUTPUTS})
