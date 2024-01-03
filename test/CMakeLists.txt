find_package(GTest REQUIRED)
target_link_libraries(GTest::GTest INTERFACE gtest_main)

file(GLOB SRC_FILES test_*.cpp)
add_executable(test_all ${SRC_FILES})
target_link_libraries(test_all PRIVATE GTest::GTest anyhttp)
