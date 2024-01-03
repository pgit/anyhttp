/**
 *
 */
#pragma once

#include <boost/asio/any_io_executor.hpp>

namespace anyhttp::server
{

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
};

class Server
{
public:
   Server(boost::asio::any_io_executor executor, Config config);
   ~Server();

private:
   class Impl;
   std::unique_ptr<Impl> impl;
};

} // namespace anyhttp::server
