//
// corosio: stepping an io_context with run_one() changes the order in which handlers run.
//
// capy  a372a6b054261f29497ac0d19c2a9533f9eaad40
// corosio 6a3eed4656e84ad6a33e74d1c8f9273d38b876ca (epoll reactor)
//
// `while (ctx.run_one()) {}` should do what `ctx.run()` does, one handler per call. It does not:
// every run()/run_one() call pushes a fresh reactor_scheduler_context frame, and that frame holds
// the inline completion budget. run() keeps one frame for all the handlers it runs, so budget
// granted by one socket completion is still there for the next handler. run_one() starts every
// handler with a budget of zero: an operation that could complete at once is posted instead,
// and whatever is queued runs before the coroutine is resumed.
//
// Below, a writer resumed by a post writes one byte, which the kernel always takes at once. A
// ticker coroutine counts its turns; when the count changes across the write, the write was
// posted ('p'), else it completed inline ('i').
//
// Output:
//
//   run():          piiiipiiiipi  (45 handlers)
//   run_one() loop: pppppppppppp  (63 handlers)
//
// Under run(), a posted write completes through complete_io_op(), which resets the budget (to 4,
// the "unassisted" budget, as the ticker is always queued); the next four writes use it up. Under
// run_one(), that budget is gone with the frame when the call returns.
//
// It matters when debugging: stepping a context to see which handler does what shows a different
// order of events than the program has when it runs normally.
//
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <coroutine>
#include <print>
#include <string>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace
{

/// Resumes the awaiting coroutine through a post to its executor, like a completion from
/// anything other than socket I/O (a timer, an event, another coroutine) does.
struct yield
{
   capy::continuation continuation;

   bool await_ready() const noexcept { return false; }
   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, const capy::io_env* env)
   {
      continuation.h = h;
      env->executor.post(continuation);
      return std::noop_coroutine();
   }
   void await_resume() const noexcept {}
};

struct State
{
   int ticks = 0;
   bool done = false;
   std::string trace;
};

capy::task<void> ticker(State& state)
{
   while (!state.done)
   {
      ++state.ticks;
      co_await yield{};
   }
}

capy::task<void> writer(corosio::local_stream_socket& socket, State& state)
{
   const char byte = 'x';
   for (int i = 0; i < 12; ++i)
   {
      co_await yield{};
      const int ticks = state.ticks;
      auto [ec, n] = co_await socket.write_some(capy::const_buffer(&byte, 1));
      if (ec || n != 1)
         std::println("write_some: {}", ec.message());
      state.trace += state.ticks == ticks ? 'i' : 'p';
   }
   state.done = true;
}

void test(bool step)
{
   corosio::io_context ctx(1);
   corosio::local_stream_socket a(ctx), b(ctx);
   if (auto ec = corosio::connect_pair(a, b))
   {
      std::println("connect_pair: {}", ec.message());
      return;
   }

   State state;
   capy::run_async(ctx.get_executor())(writer(a, state));
   capy::run_async(ctx.get_executor())(ticker(state));

   std::size_t handlers = 0;
   if (step)
      while (ctx.run_one())
         ++handlers;
   else
      handlers = ctx.run();

   std::println("{:16}{}  ({} handlers)", step ? "run_one() loop:" : "run():", state.trace,
                handlers);
}

} // namespace

int main()
{
   test(false);
   test(true);
}
