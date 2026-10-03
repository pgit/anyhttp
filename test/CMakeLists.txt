#
# GTest is built from source, with the same compiler and standard library as the tree: a prebuilt
# one is tied to one of libc++ / libstdc++ (and is not instrumented for the sanitizers).
#
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

link_libraries(Boost::atomic Boost::url Boost::filesystem Boost::process)

#
# Both API styles build every test file: one that belongs to a single style, or has parts that do,
# guards them with ANYHTTP_COROSIO.
#
file(GLOB SRC_FILES test_*.cpp)

add_executable(test_all ${SRC_FILES})
target_link_libraries(test_all PRIVATE GTest::gtest_main GTest::gmock)
target_link_libraries(test_all PRIVATE spdlog::spdlog_header_only)

target_link_libraries(test_all PRIVATE anyhttp)

# the h2c upgrade tests drive an HTTP/2 client session by hand
target_link_libraries(test_all PRIVATE PkgConfig::NGHTTP2)

# the tests connect over TLS and pass pki/out/root.pem to curl, so the PKI must exist first
add_dependencies(test_all pki)

# Register the tests with CTest
include(GoogleTest)
gtest_discover_tests(test_all
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DISCOVERY_MODE PRE_TEST 
)

# Add a custom target to run all tests
# add_custom_target(tests
#   COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
#   DEPENDS test_all
# )


# Register test_all with CTest, ensuring it runs from the project root (for External.* tests)
# add_test(NAME test_all COMMAND test_all)
# set_tests_properties(test_all PROPERTIES
#   WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
#   PASS_REGULAR_EXPRESSION "PASSED"
# )
