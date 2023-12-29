cmake_minimum_required(VERSION 3.17)
project(anyhttp_asio VERSION 0.0.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS On)
set(CMAKE_CXX_STANDARD 20)

link_libraries(Threads::Threads)
link_libraries(OpenSSL::SSL)
link_libraries(Boost::system Boost::thread Boost::atomic Boost::coroutine Boost::url)
link_libraries(nghttp2 nghttp2_asio)
link_libraries(fmt::fmt)

add_executable(hello hello.cpp)
