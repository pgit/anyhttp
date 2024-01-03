#include <boost/asio/io_context.hpp>
#include <anyhttp/server.hpp>
#include <gtest/gtest.h>

TEST(Server, HelloWorld)
{
   boost::asio::io_context context;
   anyhttp::server::Server server(context.get_executor(), {.port = 0});
   // context.run();
}
