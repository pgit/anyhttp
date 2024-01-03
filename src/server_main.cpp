
#include <boost/asio/io_context.hpp>
#include "anyhttp/server.hpp"

int main()
{
   boost::asio::io_context context;
   anyhttp::server::Server server(context.get_executor(), {});
   context.run();
   return 0;
}
