//
// What anyhttp/runtime.hpp offers generic code, checked on the runtime of the tree: the same
// expectations on ASIO and COROSIO, with only the way a test cancels something told apart.
//
#include "anyhttp/runtime.hpp"

#if ANYHTTP_ASIO
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#else
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>

#include <stop_token>
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <optional>
#include <vector>

using namespace anyhttp;
using namespace std::chrono_literals;
using namespace testing;

namespace
{

using Clock = std::chrono::steady_clock;

/// Longer than any test here may take: a delay of this that is not cancelled fails the test.
constexpr auto forever = 10s;

#if ANYHTTP_ASIO
using IoContext = boost::asio::io_context;

/// Runs a task that is cancelled from outside, the way `cancel_after` does it: by emitting
/// \p type on the cancellation slot bound to its completion.
class Cancellable
{
public:
   explicit Cancellable(
      boost::asio::cancellation_type type = boost::asio::cancellation_type::terminal)
      : type_(type)
   {
   }

   void start(const Executor& executor, Task<error_code> task, std::optional<error_code>& result)
   {
      boost::asio::co_spawn(
         executor, std::move(task),
         boost::asio::bind_cancellation_slot(
            signal_.slot(), [&result](const std::exception_ptr& ep, error_code ec) {
               if (ep)
                  ADD_FAILURE() << "threw instead of returning an error";
               else
                  result = ec;
            }));
   }

   void cancel() { signal_.emit(type_); }

private:
   boost::asio::cancellation_type type_;
   boost::asio::cancellation_signal signal_;
};
#else
using IoContext = boost::corosio::io_context;

/// Runs a task that is cancelled from outside: through the stop token it is run with.
class Cancellable
{
public:
   void start(const Executor& executor, Task<error_code> task, std::optional<error_code>& result)
   {
      auto run = [](Task<error_code> task, std::optional<error_code>& result) -> Task<void> {
         result = co_await std::move(task);
      };
      boost::capy::run_async(executor, stop_.get_token())(run(std::move(task), result));
   }

   void cancel() { stop_.request_stop(); }

private:
   std::stop_source stop_;
};
#endif

} // namespace

// =================================================================================================

TEST(Runtime, WHEN_delay_is_cancelled_THEN_it_completes_right_away_with_canceled)
{
   IoContext context;
   Cancellable cancellable;
   std::optional<error_code> result;

   cancellable.start(context.get_executor(), delay(forever), result);
   run_later(context.get_executor(), [&] { cancellable.cancel(); }); // once the timer waits
   const auto start = Clock::now();
   context.run();

   EXPECT_LT(Clock::now() - start, 1s);
   ASSERT_TRUE(result);
   EXPECT_EQ(*result, errc::operation_canceled) << result->message();
}

TEST(Runtime, WHEN_delay_is_not_cancelled_THEN_it_completes_without_error_once_it_has_passed)
{
   IoContext context;
   Cancellable cancellable;
   std::optional<error_code> result;

   cancellable.start(context.get_executor(), delay(50ms), result);
   const auto start = Clock::now();
   context.run();

   EXPECT_GE(Clock::now() - start, 50ms);
   ASSERT_TRUE(result);
   EXPECT_FALSE(*result) << result->message();
}

TEST(Runtime, WHEN_the_other_task_of_when_either_finishes_first_THEN_delay_is_cancelled)
{
   IoContext context;
   std::optional<size_t> index;

   auto race = [](std::optional<size_t>& index) -> Task<void> {
      auto result = co_await when_either(delay(forever), delay(10ms));
      index = result.index();
   };
   launch(context.get_executor(), race(index));
   const auto start = Clock::now();
   context.run();

   EXPECT_LT(Clock::now() - start, 1s) << "when_either() waited for the delay it cancelled";
   ASSERT_TRUE(index);
   EXPECT_EQ(*index, 1);
}

#if ANYHTTP_ASIO
//
// There is one kind of cancellation (docs/cancellation.rst): ASIO's awaitable lets only terminal
// through, by default, so a request for partial or total alone does not reach the timer.
//
TEST(Runtime, WHEN_partial_or_total_cancellation_alone_is_requested_THEN_delay_runs_its_course)
{
   IoContext context;
   Cancellable cancellable(boost::asio::cancellation_type::partial |
                           boost::asio::cancellation_type::total);
   std::optional<error_code> result;

   cancellable.start(context.get_executor(), delay(50ms), result);
   run_later(context.get_executor(), [&] { cancellable.cancel(); });
   const auto start = Clock::now();
   context.run();

   EXPECT_GE(Clock::now() - start, 50ms);
   ASSERT_TRUE(result);
   EXPECT_FALSE(*result) << result->message();
}
#endif

// =================================================================================================

TEST(Runtime, WHEN_timer_is_rearmed_or_cancelled_THEN_only_the_last_callback_runs)
{
   IoContext context;
   Timer timer(context.get_executor());
   std::vector<int> fired;

   timer.arm(1ms, [&] { fired.push_back(1); });
   timer.arm(2ms, [&] { fired.push_back(2); });
   Timer other(context.get_executor());
   other.arm(1ms, [&] { fired.push_back(3); });
   other.cancel();
   context.run();
   EXPECT_THAT(fired, ElementsAre(2));
}
