#pragma once

#include <chrono>
#include <cstddef>
#include <print>

// =================================================================================================

//
// Runs \p context until it is out of work, like its run(). In a Debug build, it does so one
// handler at a time with run_one(), printing a separator after each -- in red when one took 10 ms
// or more. That shows which turn of the event loop each log line belongs to.
//
template <typename IoContext>
size_t run(IoContext& context)
{
#if defined(GITHUB_ACTIONS) || defined(NDEBUG)
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
         std::println("--- {} ------------------------------------------------------------------------", i);
      else
         std::println("\x1b[1;31m--- {} ({}) ----------------------------------------------------------------\x1b[0m", i, dt);
      // clang-format on
   }
   return i;
#endif
}

/// A TCP port on the IPv6 loopback that nothing listens on -- at least when this returns.
unsigned short get_unused_port();

// =================================================================================================
