#include "anyhttp/common.hpp"
#include "anyhttp/utils.hpp"

#include <format>

// =================================================================================================

size_t run(boost::asio::io_context& context)
{
#if defined(NDEBUG)
   return context.run();
#else
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
         LOG("--- {} ------------------------------------------------------------------------", i);
      else
         LOG("\x1b[1;31m--- {} ({}) ----------------------------------------------------------------\x1b[0m", i, dt);
      // clang-format off
   }
   return i;
#endif
}

// =================================================================================================
