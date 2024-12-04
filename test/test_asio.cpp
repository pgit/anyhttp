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
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/url.hpp>

#include <gtest/gtest.h>

#include <print>

#include "anyhttp/common.hpp"

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

TEST_F(Asio, SpawnDetached)
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
   context.run();
   EXPECT_TRUE(ok);
}

TEST_F(Asio, SpawnFuture)
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
   context.run();
   EXPECT_TRUE(future.get());
}

// =================================================================================================

// https://www.boost.org/doc/libs/1_86_0/doc/html/boost_asio/example/cpp20/type_erasure/sleep.hpp
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

TEST_F(ComposedAny, CustomDetached)
{
   boost::asio::io_context context;
   async_sleep(context.get_executor(), 100ms, asio::detached);
   context.run();
}

TEST_F(ComposedAny, CustomFuture)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   context.run();
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
         { async_sleep_impl(std::move(handler), std::move(ex), duration); },
         std::forward<CompletionToken>(token), std::move(ex), duration);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ComposedAny, AnyDetached)
{
   boost::asio::io_context context;
   async_sleep(context.get_executor(), 100ms, asio::detached);
   context.run();
}

TEST_F(ComposedAny, AnyFuture)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   context.run();
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
   static auto async_sleep_impl(SleepHandler token, Duration duration)
   {
      return asio::async_initiate<SleepHandler, Sleep>( //
         asio::co_composed<Sleep>(
            [](auto state, Duration duration) -> void
            {
               auto ex = state.get_io_executor();
               asio::steady_timer timer(ex, duration);
               timer.expires_from_now(100ms);
               std::println("waiting...");
               auto [ec] =
                  co_await timer.async_wait(bind_executor(ex, asio::as_tuple(asio::deferred)));
               std::println("waiting... done, {}", ec.what());
               co_return {boost::system::error_code{}};
            }),
         token, duration);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   static auto async_sleep(Duration duration, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Sleep>(
         [](SleepHandler handler, Duration duration)
         { async_sleep_impl(std::move(handler), duration); }, std::forward<CompletionToken>(token),
         duration);
   }
};

// -------------------------------------------------------------------------------------------------

//
// This one will FAIL to register work properly. The wait will somehow be carried over into
// the next test... BAD.
//
TEST_F(ComposedCoro, AnyDetached)
{
   boost::asio::io_context context;
   async_sleep(100ms, asio::detached);
   context.run();
}

TEST_F(ComposedCoro, AnyFuture)
{
   boost::asio::io_context context;
   auto future = async_sleep(100ms, asio::use_future);
   context.run();
   EXPECT_NO_THROW(future.get());
}

// =================================================================================================

//
// Simply by passing an executor to register work in, async_initiate seems to register work for us.
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
               timer.expires_from_now(100ms);
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
   context.run();
}
TEST_F(ComposedExecutor, AnyFuture)
{
   boost::asio::io_context context;
   auto future = async_sleep(context.get_executor(), 100ms, asio::use_future);
   context.run();
   EXPECT_NO_THROW(future.get());
}

// =================================================================================================

class ComposedComa : public testing::Test
{
public:
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   auto coma_composed(Duration duration, CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>( //
         asio::co_composed<Sleep>(
            [this](auto state, Duration duration) -> void
            {
               // co_await asio::this_coro::executor;
               auto ex = state.get_io_executor();
               // co_await sleep(duration);
               asio::steady_timer timer(ex, duration);
               co_await timer.async_wait(asio::bind_executor(ex, asio::deferred));
               co_return {boost::system::error_code{}};
            }),
         token, duration);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   auto coma(boost::asio::any_io_executor ex, Duration duration, CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>(
         [this](SleepHandler handler, boost::asio::any_io_executor ex, Duration duration) -> void
         {
            this->work.emplace(ex);
            this->handler = std::move(handler); //
         },
         token, ex, duration);
   }

   using WorkGuard = boost::asio::executor_work_guard<asio::any_completion_executor>;
   std::optional<WorkGuard> work;
   SleepHandler handler;

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Sleep) CompletionToken>
   auto coma(Duration duration, CompletionToken&& token)
   {
      return asio::async_initiate<CompletionToken, Sleep>(
         [this](SleepHandler handler, Duration duration) -> void
         {
            // this works only if we associate our executor with the handler before
            // but I guess that makes sense... how else would it know about the executor?
            auto ex = boost::asio::get_associated_executor(handler);
            this->work.emplace(ex);
            this->handler = std::move(handler);
         },
         token, duration);
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(ComposedComa, Coma)
{
   boost::asio::io_context context;
   coma(100ms, asio::bind_executor(context.get_executor(), asio::detached));
   // coma(100ms, asio::detached);
   std::atomic<bool> running = true;
   std::thread thread(
      [&]
      {
         context.run();
         running = false;
      });

   std::this_thread::sleep_for(50ms);
   EXPECT_TRUE(running); // if not, work was not registered properly
   post(context.get_executor(), [this]() { work.reset(); });

   thread.join();
}

TEST_F(ComposedComa, ComaPoll)
{
   boost::asio::io_context context;
   bool done = false;
   auto tok = [&](boost::system::error_code ec) { done = true; };
   coma(100ms, asio::bind_executor(context.get_executor(), tok));
   // coma(context.get_executor(), 100ms, tok);
   // coma(100ms, tok);  // FAILS, doesn't register work
   // coma_composed(100ms, asio::bind_executor(context.get_executor(), tok));
   EXPECT_EQ(context.poll(), 0);
   EXPECT_FALSE(done);
   post(context.get_executor(),
        [this]()
        {
           anyhttp::swap_and_invoke(handler, boost::system::error_code{});
           work.reset();
        });
   EXPECT_FALSE(done);
   EXPECT_EQ(context.poll(), 1);
   EXPECT_TRUE(done);
}

// =================================================================================================
