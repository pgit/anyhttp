find_package(GTest REQUIRED)
target_link_libraries(GTest::GTest INTERFACE gtest_main)

link_libraries(Boost::atomic Boost::url Boost::filesystem Boost::process)

file(GLOB SRC_FILES test_*.cpp)
add_executable(test_all ${SRC_FILES})
target_link_libraries(test_all PRIVATE GTest::GTest GTest::gmock anyhttp)
target_link_libraries(test_all PRIVATE spdlog::spdlog_header_only)

# include(CTest)
# Fetch Google Test
# include(FetchContent)
# FetchContent_Declare(
#   googletest
#   URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
# )
# FetchContent_MakeAvailable(googletest)

# Register the tests with CTest
include(GoogleTest)
gtest_discover_tests(test_all
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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
