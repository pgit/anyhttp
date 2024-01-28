cmake_minimum_required(VERSION 3.17)
project(anyhttp VERSION 0.0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS On)
set(CMAKE_CXX_STANDARD 20)

include_directories(${CMAKE_SOURCE_DIR}/include)

link_libraries(Threads::Threads)
link_libraries(OpenSSL::SSL)
link_libraries(Boost::system Boost::thread Boost::atomic Boost::coroutine Boost::url)
link_libraries(nghttp2 nghttp2_asio)
link_libraries(fmt::fmt)
# link_libraries(fmtlog-static.a)
link_libraries(spdlog::spdlog_header_only)

add_executable(hello hello.cpp)

add_library(anyhttp)
file(GLOB anyhttp_sources CONFIGURE_DEPENDS *.cpp)
target_sources(anyhttp PRIVATE ${anyhttp_sources})
target_include_directories(anyhttp PUBLIC ../include)

add_executable(server)
target_sources(server PRIVATE server_main.cpp)
target_link_libraries(server PRIVATE anyhttp)

add_executable(client)
target_sources(client PRIVATE client_main.cpp)
target_link_libraries(client PRIVATE anyhttp)
