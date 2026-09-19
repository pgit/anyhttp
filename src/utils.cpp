#include "anyhttp/utils.hpp"

#include <boost/asio/ip/tcp.hpp>

// =================================================================================================

#if defined(GITHUB_ACTIONS) || defined(NDEBUG)
size_t run(boost::asio::io_context& context) { return context.run(); }
#else
#include <print>
size_t run(boost::asio::io_context& context)
{
   size_t i = 0;
   using namespace std::chrono;
   auto t0 = steady_clock::now();
   for (i = 0; context.run_one(); ++i)
   {
      auto t1 = steady_clock::now();
      auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      // clang-format off
      if (dt < 10ms)
         std::println("--- {} ------------------------------------------------------------------------", i);
      else
         std::println("\x1b[1;31m--- {} ({}) ----------------------------------------------------------------\x1b[0m", i, dt);
      // clang-format on
   }
   return i;
}
#endif

// =================================================================================================

unsigned short get_unused_port(boost::asio::io_context& io)
{
   using namespace boost::asio::ip;

   tcp::acceptor acc(io);
   acc.open(tcp::v6());
   acc.bind({address_v6::loopback(), 0});

   auto port = acc.local_endpoint().port();

   acc.close(); // release immediately

   return port;
}

// =================================================================================================
