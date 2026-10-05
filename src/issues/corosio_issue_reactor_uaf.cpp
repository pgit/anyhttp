//
// corosio: heap-use-after-free in the epoll reactor with more than one thread in run() (not filed
// yet; corosio 6a3eed46).
//
// epoll_scheduler::run_task() takes each event's reactor_descriptor_state* from epoll_wait() and
// calls add_ready_events() on it, then sets is_enqueued_, without holding the descriptor's mutex.
// That state is part of the socket's implementation. Destroying the socket on another thread
// meanwhile -- reactor_socket_service::destroy() -> do_close_socket() -> erase from impl_ptrs_ --
// frees it: do_close_socket() pins the implementation through impl_ref_ only when is_enqueued_ is
// already set, which the reactor thread has not done yet. epoll_ctl(EPOLL_CTL_DEL) does not help,
// the event is already in the reactor's buffer.
//
// The reproducer runs a few coroutines on a context with several threads. Each one creates a
// connected socket pair, writes a byte so that epoll reports the peer readable, and destroys both
// sockets right away -- all on the context, as any server does when a connection ends. One thread
// sits in epoll_wait() while the others run the coroutines.
//
// Built with -fsanitize=address, it reports a heap-use-after-free in add_ready_events(), freed by
// reactor_socket_service::destroy(), within a second. Without a sanitizer, most runs on 4
// threads stall part way, which the watchdog reports ("stuck after N of M rounds"): presumably
// the stale event lands on the next socket allocated at the same address, whose write then never
// completes. With one thread ("corosio_issue_reactor_uaf 1") it always completes.
//
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

static std::atomic<int> rounds_done{0};

static capy::task<void> churn(corosio::io_context& context, int rounds)
{
   for (int i = 0; i < rounds; ++i)
   {
      corosio::local_stream_socket a(context), b(context);
      if (auto ec = corosio::connect_pair(a, b))
      {
         std::fprintf(stderr, "connect_pair: %s\n", ec.message().c_str());
         std::abort();
      }

      // Makes 'a' readable: some thread in epoll_wait() gets an event for it.
      char byte = 'x';
      auto [ec, n] = co_await b.write_some(capy::const_buffer(&byte, 1));
      if (ec)
      {
         std::fprintf(stderr, "write_some: %s\n", ec.message().c_str());
         std::abort();
      }

      ++rounds_done;
      // 'a' and 'b' are destroyed here, while that event may still be on its way.
   }
}

int main(int argc, char* argv[])
{
   unsigned threads = argc > 1 ? std::atoi(argv[1]) : 4;
   int coroutines = 16;
   int rounds = 20000;

   corosio::io_context context(threads);
   for (int i = 0; i < coroutines; ++i)
      capy::run_async(context.get_executor())(churn(context, rounds));

   std::vector<std::jthread> runners;
   for (unsigned i = 1; i < threads; ++i)
      runners.emplace_back([&] { context.run(); });

   // Reports when the rounds stop making progress, and gives up.
   std::jthread watchdog([&](std::stop_token stop) {
      int last = -1;
      while (!stop.stop_requested())
      {
         std::this_thread::sleep_for(std::chrono::seconds(2));
         int now = rounds_done.load();
         if (now == last && !stop.stop_requested())
         {
            std::fprintf(stderr, "stuck after %d of %d rounds\n", now, coroutines * rounds);
            std::_Exit(2);
         }
         last = now;
      }
   });

   context.run();
   runners.clear();
   watchdog.request_stop();

   std::printf("%d rounds on %u threads, done\n", rounds_done.load(), threads);
   return 0;
}
