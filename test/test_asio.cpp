#include "anyhttp/common.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <print>
#include <thread>

using namespace std::chrono_literals;
namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators;

// =================================================================================================

template <typename T>
boost::asio::awaitable<void> sleep(T duration)
{
   asio::steady_timer timer(co_await asio::this_coro::executor, duration);
   co_await timer.async_wait(asio::deferred);
}

using Sleep = void(boost::system::error_code);
using SleepHandler = asio::any_completion_handler<Sleep>;

using Duration = std::chrono::nanoseconds;

// =================================================================================================

class Asio : public testing::Test
{
};

// -------------------------------------------------------------------------------------------------

TEST_F(Asio, DISABLED_HttpDateBenchmark)
{
   for (size_t i = 0; i < 1'000'000; ++i)
      format_http_date(std::chrono::system_clock::now());
}

TEST_F(Asio, WHEN_task_is_spawned_THEN_work_is_tracked)
{
   boost::asio::io_context context;
   bool ok = false;
   co_spawn(
      context.get_executor(),
      [&]() -> asio::awaitable<void>
      {
         co_await sleep(100ms);
         ok = true;
         co_return;
      },
      asio::detached);
   ::run(context);
   EXPECT_TRUE(ok);
}

TEST_F(Asio, WHEN_task_is_finished_THEN_sets_future)
{
   boost::asio::io_context context;
   auto future = co_spawn(
      context.get_executor(),
      [&]() -> asio::awaitable<bool>
      {
         co_await sleep(100ms);
         co_return true;
      },
      asio::use_future);
   ::run(context);
   EXPECT_TRUE(future.get());
}

// =================================================================================================

/// https://www.boost.org/doc/libs/1_86_0/doc/html/boost_asio/example/cpp20/type_erasure/sleep.hpp
class ComposedAny : public testing::Test
{
public:
   static void async_sleep_impl(SleepHandler handler, boost::asio::any_io_executor ex,
                                Duration duration)
   {
      auto timer = std::make_shared<boost::asio::steady_timer>(ex, duration);
      timer->async_wait(boost::asio::consign(std::move(handler), timer));
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   static inline auto async_sleep(boost::asio::any_io_executor ex, Duration duration,
                                  CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Sleep>(async_sleep_impl, token,
                                                                 std::move(ex), duration);
   }
};

TEST_F(ComposedAny, WHEN_async_op_is_initiated_THEN_tracks_work)
{
   boost::asio::io_context context;
   async_sleep(context.get_executor(), 100ms, asio::detached);
   ::run(context);
}

TEST_F(ComposedAny, WHEN_async_op_finishes_THEN_sets_future)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   ::run(context);
   EXPECT_NO_THROW(future.get());
}

// -------------------------------------------------------------------------------------------------

class ComposedIndirect : public testing::Test
{
public:
   static auto async_sleep_impl(SleepHandler token, boost::asio::any_io_executor ex,
                                Duration duration)
   {
      return asio::async_initiate<SleepHandler, Sleep>(
         [](auto handler, boost::asio::any_io_executor ex, Duration duration)
         {
            auto timer = std::make_shared<asio::steady_timer>(ex, duration);
            return timer->async_wait(consign(std::move(handler), timer));
         },
         token, std::move(ex), duration);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   static auto async_sleep(boost::asio::any_io_executor ex, Duration duration,
                           CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Sleep>(
         [](SleepHandler handler, boost::asio::any_io_executor ex, Duration duration)
         {
            async_sleep_impl(std::move(handler), std::move(ex), duration); //
         },
         std::forward<CompletionToken>(token), std::move(ex), duration);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ComposedIndirect, WHEN_async_op_is_initiated_THEN_tracks_work)
{
   boost::asio::io_context context;
   async_sleep(context.get_executor(), 100ms, asio::detached);
   ::run(context);
}

TEST_F(ComposedIndirect, WHEN_async_op_finishes_THEN_sets_future)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   ::run(context);
   EXPECT_NO_THROW(future.get());
}

// =================================================================================================

//
// This variant using co_composed<> does not register work properly: If used with a 'detached'
// completion token, the sleep will not register work in the executor. At least not in the one
// fetchted using get_io_executor()...
//
class ComposedCoro : public testing::Test
{
public:
   boost::asio::io_context context;

   template <typename CompletionToken>
   auto async_sleep(Duration duration, CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>( //
         asio::co_composed<Sleep>(
            [this](auto state, Duration duration) -> void
            {
               auto ex = state.get_io_executor();
               auto thread_id = std::this_thread::get_id();
               std::println("waiting in thread {}...", thread_id);
               // asio::steady_timer timer(ex, duration);
               // auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::deferred));
               co_await asio::steady_timer(ex, duration).async_wait(asio::deferred);
               if (thread_id == std::this_thread::get_id())
                  std::println("waiting in thread {}... done", thread_id);
               else
                  std::println("waiting in thread {}... done, but now in {}!", thread_id,
                               std::this_thread::get_id());
               ++done;               
               co_return {boost::system::error_code{}};
            }),
         token, duration);
   }

   // std::atomic<size_t> done = 0;
   size_t done = 0;
};

// -------------------------------------------------------------------------------------------------

//
// This one does not register work properly, because async_sleep() is called with a token that
// is NOT bound to the executor. The async operation will then uses the system executor as fallback.
//
// If this test is run and other tests follow that keep asio running for 100ms, then the
// asynchronous operation will continue to run. Eventually, it will call ++done, which is
// a use-after-free.
//
TEST_F(ComposedCoro, DISABLED_Unbound)
{
   boost::asio::io_context context;
   async_sleep(100ms, asio::detached);
   ::run(context);
   EXPECT_EQ(done, 1);
}

//
// This test is only (somewhat) safe because there is a longer sleep in the test that follows.
// If you run it under very high load, it may still fail, very seldomly.
//
TEST_F(ComposedCoro, DefaultExecutor)
{
   boost::asio::io_context context;
   async_sleep(100ms, asio::detached);  // this not safe, does not register work properly
   async_sleep(120ms, bind_executor(context, asio::detached));
   ::run(context);
   EXPECT_EQ(done, 2);
}

TEST_F(ComposedCoro, AnyDetached)
{
   boost::asio::io_context context;
   async_sleep(100ms, bind_executor(context, asio::detached));
   async_sleep(100ms, bind_executor(context, asio::detached));
   ::run(context);
   EXPECT_EQ(done, 2);
}

//
// Also with futures, we need to bind the executor to the completion token. If we don't do this,
// there is a rare chance that the async operation will complete after the test has finished. The
// handler is executed in the system executor, which is out of our control. TSAN shows this, at
// least sometimes.
//
TEST_F(ComposedCoro, AnyFuture)
{
   boost::asio::io_context context;
   auto f1 = async_sleep(100ms, bind_executor(context, asio::use_future));
   auto f2 = async_sleep(100ms, bind_executor(context, asio::use_future));
   ::run(context);
   EXPECT_NO_THROW(f1.get());
   EXPECT_NO_THROW(f2.get());
   EXPECT_EQ(done, 2);
}

// =================================================================================================

//
// Another way to pass the executor to the composed function is to pass the executor as an argument.
//
class ComposedExecutor : public testing::Test
{
public:
   static auto async_sleep_impl(SleepHandler token, boost::asio::any_io_executor ex,
                                Duration duration)
   {
      return asio::async_initiate<SleepHandler, Sleep>( //
         asio::co_composed<Sleep>(
            [](auto state, boost::asio::any_io_executor ex, Duration duration) -> void
            {
               asio::steady_timer timer(ex, duration);
               timer.expires_after(100ms);
               std::println("waiting (with ex)...");
               auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::deferred));
               std::println("waiting (with ex)... done, {}", ec.what());
               co_return {boost::system::error_code{}};
            }),
         token, ex, duration);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   static auto async_sleep(boost::asio::any_io_executor ex, Duration duration,
                           CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Sleep>(
         [](SleepHandler handler, boost::asio::any_io_executor ex, Duration duration)
         {
            async_sleep_impl(std::move(handler), ex, duration); //
         },
         std::forward<CompletionToken>(token), std::move(ex), duration);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ComposedExecutor, AnyDetached)
{
   boost::asio::io_context context;
   async_sleep(context.get_executor(), 100ms, asio::detached);
   ::run(context);
}
TEST_F(ComposedExecutor, AnyFuture)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   ::run(context);
   EXPECT_NO_THROW(future.get());
}

// =================================================================================================

class ComposedHandler : public testing::Test
{
public:
   using WorkGuard = boost::asio::executor_work_guard<asio::any_completion_executor>;
   std::optional<WorkGuard> work;
   SleepHandler handler;

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   auto wait_for_handler(CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>(
         [this](SleepHandler handler) -> void
         {
            // this works only if we associate our executor with the handler before
            // but I guess that makes sense... how else would it know about the executor?
            auto ex = boost::asio::get_associated_executor(handler);
            this->handler = std::move(handler);
         },
         token);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   auto wait_for_handler_with_work(CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>(
         [this](SleepHandler handler) -> void
         {
            auto ex = boost::asio::get_associated_executor(handler);
            this->work.emplace(ex);
            this->handler = std::move(handler);
         },
         token);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ComposedHandler, Coma)
{
   boost::asio::io_context context;
   wait_for_handler(asio::bind_executor(context.get_executor(), asio::detached));
   EXPECT_EQ(context.run_for(100ms), 0);
   dispatch(context.get_executor(), [handler = std::move(handler)]() mutable
            { std::move(handler)(boost::system::error_code{}); });
   EXPECT_EQ(::run(context), 0);
}

TEST_F(ComposedHandler, ComaPoll)
{
   boost::asio::io_context context;
   wait_for_handler_with_work(asio::bind_executor(context.get_executor(), asio::detached));
   EXPECT_EQ(context.run_for(100ms), 0);
   dispatch(context.get_executor(), [handler = std::move(handler)]() mutable
            { std::move(handler)(boost::system::error_code{}); });
   work.reset();
   EXPECT_EQ(::run(context), 1);
}

// =================================================================================================
