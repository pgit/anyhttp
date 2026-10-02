#pragma once

//
// The runtime layer for capy/corosio: coroutines only, capy::task, capy::any_executor,
// std::stop_token for cancellation. See anyhttp/runtime.hpp for what this is.
// detail/runtime_asio.hpp defines the same names for Boost.Asio and has their documentation; what
// is said here is what differs.
//
// Only single-threaded use is supported: everything of a server or client runs on one thread.
//

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/asio.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/when_any.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/wait_type.hpp>

// the buffer vocabulary stays Boost.Asio's, which Beast needs anyway
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/error_code.hpp>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <sys/socket.h>

namespace anyhttp
{
namespace asio = boost::asio;
namespace capy = boost::capy;
namespace corosio = boost::corosio;

// =================================================================================================

template <typename T = void>
using Task = capy::task<T>;

using Executor = capy::any_executor;

using std::error_code;

/// Everything of a server or client runs on one thread: there are no strands.
inline constexpr bool multithreaded_runtime = false;

/// What the runtime throws an error_code as.
using system_error = std::system_error;

// -------------------------------------------------------------------------------------------------

/// Portable error conditions, see detail/runtime_asio.hpp.
using errc = std::errc;

namespace errors
{
inline const error_code eof = capy::error::eof;
inline const error_code partial_message = capy::error::stream_truncated;
inline const error_code canceled = capy::error::canceled;
inline const error_code would_block = std::make_error_code(std::errc::operation_would_block);
inline const error_code connection_aborted = std::make_error_code(std::errc::connection_aborted);
inline const error_code bad_descriptor = std::make_error_code(std::errc::bad_file_descriptor);
inline const error_code already_started =
   std::make_error_code(std::errc::connection_already_in_progress);
inline const error_code header_limit =
   boost::system::error_code(boost::beast::http::error::header_limit);
} // namespace errors

[[noreturn]] inline void throw_error(const error_code& ec) { throw std::system_error(ec); }

inline error_code last_error() noexcept { return {errno, std::system_category()}; }

// =================================================================================================

namespace detail
{

/**
 * Where a parked operation leaves its result, and how its caller gets resumed. It is part of the
 * awaitable initiate() returns, which lives in the caller's coroutine frame for as long as the
 * operation is in progress.
 */
template <typename... Args>
struct CompletionState
{
   capy::continuation continuation;
   const capy::io_env* env = nullptr;
   std::optional<std::tuple<error_code, Args...>> result;
   std::optional<std::stop_callback<std::function<void()>>> on_stop;

   /// Never resumes the caller inline: it is posted to the caller's executor.
   void complete(error_code ec, Args... args)
   {
      result.emplace(ec, std::move(args)...);
      on_stop.reset();
      env->executor.post(continuation);
   }
};

template <typename Signature>
struct CompletionTraits;

template <typename... Args>
struct CompletionTraits<void(error_code, Args...)>
{
   using State = CompletionState<Args...>;
   using Result = std::tuple<error_code, Args...>;
};

} // namespace detail

template <typename Signature>
class Completion;

/**
 * An operation that has been started and not completed yet, see detail/runtime_asio.hpp. Here,
 * it refers to the state of the awaitable its caller waits on. Invoking it posts the caller's
 * resumption, it never resumes it inline.
 *
 * One that is destroyed without having been invoked completes as cancelled, so that nobody waits
 * for it forever.
 */
template <typename... Args>
class Completion<void(error_code, Args...)>
{
public:
   using State = detail::CompletionState<Args...>;

   Completion() noexcept = default;
   Completion(std::nullptr_t) noexcept {}
   explicit Completion(State* state) noexcept : state_(state) {}
   Completion(Completion&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
   Completion& operator=(Completion&& other) noexcept
   {
      if (this != &other)
      {
         abandon();
         state_ = std::exchange(other.state_, nullptr);
      }
      return *this;
   }
   Completion& operator=(std::nullptr_t) noexcept
   {
      abandon();
      return *this;
   }
   ~Completion() { abandon(); }

   explicit operator bool() const noexcept { return state_ != nullptr; }

   void operator()(error_code ec, Args... args) &&
   {
      std::exchange(state_, nullptr)->complete(ec, std::move(args)...);
   }

   State* state() const noexcept { return state_; }

private:
   void abandon() noexcept
   {
      if (state_)
         std::exchange(state_, nullptr)->complete(errors::canceled, Args{}...);
   }

   State* state_ = nullptr;
};

// =================================================================================================

template <typename Handler, typename... Args>
inline void complete_immediately(Handler&& handler, const Executor&, Args&&... args)
{
   if (handler)
      std::move(handler)(std::forward<Args>(args)...);
}

template <typename Handler, typename... Args>
inline void complete_later(Handler&& handler, const Executor&, Args&&... args)
{
   if (handler)
      std::move(handler)(std::forward<Args>(args)...);
}

/**
 * Calls \p on_cancel when the caller's stop token is triggered. A stop requested before the
 * operation was started never gets here: initiate() completes such an operation as cancelled
 * right away.
 */
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto* state = handler.state();
   if (!state || !state->env->stop_token.stop_possible())
      return;
   state->on_stop.emplace(state->env->stop_token,
                          std::function<void()>(std::forward<F>(on_cancel)));
}

// -------------------------------------------------------------------------------------------------

namespace detail
{

template <typename Signature, typename Init>
class Initiation
{
   using Traits = CompletionTraits<Signature>;

public:
   explicit Initiation(Init init) : init_(std::move(init)) {}

   //
   // IoAwaitable wants the awaitable movable, and moving it is only ever done before it is
   // awaited, while its state is still empty: that has a stop_callback, which does not move.
   //
   Initiation(Initiation&& other) noexcept(std::is_nothrow_move_constructible_v<Init>)
      : init_(std::move(other.init_))
   {
   }

   bool await_ready() const noexcept { return false; }

   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h, const capy::io_env* env)
   {
      state_.continuation.h = h;
      state_.env = env;
      if (env->stop_token.stop_requested())
      {
         canceled(std::type_identity<typename Traits::Result>{});
         return h;
      }
      init_(Completion<Signature>(&state_));
      return std::noop_coroutine();
   }

   typename Traits::Result await_resume() { return std::move(*state_.result); }

private:
   template <typename... Args>
   void canceled(std::type_identity<std::tuple<error_code, Args...>>)
   {
      state_.result.emplace(errors::canceled, Args{}...);
   }

   Init init_;
   typename Traits::State state_;
};

} // namespace detail

/// Starts an operation that completes a <tt>Completion<Signature></tt>, and waits for it.
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return detail::Initiation<Signature, std::decay_t<Init>>(std::forward<Init>(init));
}

// -------------------------------------------------------------------------------------------------

/// Calls \p function from \p executor's queue, after whatever is running now has returned.
template <typename F>
inline void run_later(const Executor& executor, F&& function);

/// Only one thread runs everything here, so there is nothing to hop onto: calls \p function now.
template <typename F>
inline void dispatch_to(const Executor&, F&& function)
{
   std::forward<F>(function)();
}

/// There are no strands: everything runs on one thread (server::Config::use_strand is refused).
inline Executor new_strand(const Executor& executor) { return executor; }

/// Starts \p task on \p executor, detached: nobody waits for it, and what it throws is dropped.
inline void launch(const Executor& executor, Task<void> task)
{
   capy::run_async(executor, []() noexcept {}, [](std::exception_ptr) noexcept {})(std::move(task));
}

/// Starts \p task on \p executor and calls \p on_done with what it threw, if anything.
template <typename F>
inline void launch(const Executor& executor, Task<void> task, F&& on_done)
{
   auto done = std::make_shared<std::decay_t<F>>(std::forward<F>(on_done));
   capy::run_async(
      executor, [done]() noexcept { (*done)(std::exception_ptr{}); },
      [done](std::exception_ptr ep) noexcept { (*done)(ep); })(std::move(task));
}

} // namespace anyhttp

/// The error code of what \p ptr holds, as thrown by the runtime.
inline std::error_code code(const std::exception_ptr& ptr)
{
   if (!ptr)
      return {};
   try
   {
      std::rethrow_exception(ptr);
   }
   catch (const std::system_error& ex)
   {
      return ex.code();
   }
   catch (const boost::system::system_error& ex)
   {
      return ex.code();
   }
}

namespace anyhttp
{

/**
 * Runs \p task as the operation \p handler stands for. The caller's stop token becomes the task's,
 * so cancelling the caller cancels the task.
 */
template <typename... T>
inline void launch(const Executor& executor, Task<std::tuple<error_code, T...>> task,
                   Completion<void(error_code, T...)>&& handler)
{
   std::stop_token token;
   if (auto* state = handler.state())
      token = state->env->stop_token;

   auto shared = std::make_shared<Completion<void(error_code, T...)>>(std::move(handler));
   capy::run_async(
      executor, token,
      [shared](std::tuple<error_code, T...> result) noexcept {
         std::apply(std::move(*shared), std::move(result));
      },
      [shared](std::exception_ptr ep) noexcept { std::move (*shared)(code(ep), T{}...); })(
      std::move(task));
}

// -------------------------------------------------------------------------------------------------

/// Lets whatever else is ready to run on the caller's executor run first.
struct YieldNow
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

inline YieldNow yield_now() { return {}; }

template <typename F>
inline void run_later(const Executor& executor, F&& function)
{
   launch(executor, [](std::decay_t<F> function) -> Task<void> {
      co_await yield_now();
      function();
   }(std::forward<F>(function)));
}

/// Waits for \p duration to pass, or until cancelled (errors::canceled).
inline Task<error_code> delay(std::chrono::steady_clock::duration duration)
{
   auto [ec] = co_await corosio::delay(duration);
   co_return ec;
}

namespace detail
{
/// What when_both() yields: as ASIO's awaitable operator && does.
template <typename A, typename B>
using both_t = std::conditional_t<std::is_void_v<A>, B,
                                  std::conditional_t<std::is_void_v<B>, A, std::tuple<A, B>>>;

/// What a task returns, with \c std::monostate for nothing.
template <typename T>
using value_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/**
 * \p task as what capy::when_all() and capy::when_any() take, an operation with an io_result,
 * keeping its value. What it throws passes through.
 */
template <typename T>
capy::io_task<> as_io(Task<T> task, std::optional<value_t<T>>& result)
{
   if constexpr (std::is_void_v<T>)
   {
      co_await std::move(task);
      result.emplace();
   }
   else
      result.emplace(co_await std::move(task));
   co_return capy::io_result<>{};
}
} // namespace detail

/// Runs \p a and \p b concurrently, until both are done, see detail/runtime_asio.hpp.
template <typename A, typename B>
Task<detail::both_t<A, B>> when_both(Task<A> a, Task<B> b)
{
   std::optional<detail::value_t<A>> ra;
   std::optional<detail::value_t<B>> rb;
   std::ignore =
      co_await capy::when_all(detail::as_io(std::move(a), ra), detail::as_io(std::move(b), rb));
   if constexpr (std::is_void_v<A> && std::is_void_v<B>)
      co_return;
   else if constexpr (std::is_void_v<A>)
      co_return std::move(*rb);
   else if constexpr (std::is_void_v<B>)
      co_return std::move(*ra);
   else
      co_return std::tuple{std::move(*ra), std::move(*rb)};
}

/**
 * Runs \p a and \p b concurrently, until one of them succeeds, see detail/runtime_asio.hpp. That
 * is what capy::when_any() does: a child that throws does not win, and when none has won, it
 * rethrows what one of them threw.
 */
template <typename A, typename B>
Task<std::variant<detail::value_t<A>, detail::value_t<B>>> when_either(Task<A> a, Task<B> b)
{
   std::optional<detail::value_t<A>> ra;
   std::optional<detail::value_t<B>> rb;
   auto winner =
      co_await capy::when_any(detail::as_io(std::move(a), ra), detail::as_io(std::move(b), rb));
   using result = std::variant<detail::value_t<A>, detail::value_t<B>>;
   switch (winner.index())
   {
   case 1:
      co_return result{std::in_place_index<0>, std::move(*ra)};
   case 2:
      co_return result{std::in_place_index<1>, std::move(*rb)};
   default:
      throw_error(std::get<0>(winner)); // as_io() never reports an error code
   }
}

/// A stop request can not be taken back: nothing to reset, see detail/runtime_asio.hpp.
inline Task<void> reset_cancellation() { co_return; }

/// Awaits \p task with a stop token of its own, see detail/runtime_asio.hpp.
template <typename T>
Task<T> shielded(Task<T> task)
{
   co_return co_await capy::run(std::stop_token{})(std::move(task));
}

// =================================================================================================

/// capy's async_event, see detail/runtime_asio.hpp.
class Event
{
public:
   void set() { event_.set(); }
   void clear() noexcept { event_.clear(); }
   bool is_set() const noexcept { return event_.is_set(); }

   /// <tt>auto [ec] = co_await event.wait();</tt>
   auto wait() { return event_.wait(); }

private:
   capy::async_event event_;
};

// -------------------------------------------------------------------------------------------------

/**
 * A timer that calls back when it expires, see detail/runtime_asio.hpp. Each arm() starts a wait
 * of its own, which the next arm() or cancel() stops.
 */
class Timer
{
public:
   explicit Timer(const Executor& executor) : executor_(executor) {}
   Timer(const Timer&) = delete;
   Timer& operator=(const Timer&) = delete;
   ~Timer() { cancel(); }

   template <typename Rep, typename Period, typename F>
   void arm(std::chrono::duration<Rep, Period> delay, F&& on_expiry)
   {
      cancel();
      stop_.emplace();
      auto wait = [](std::chrono::steady_clock::duration delay,
                     std::function<void()> on_expiry) -> Task<void> {
         if (auto [ec] = co_await corosio::delay(delay); !ec)
            on_expiry();
      };
      capy::run_async(
         executor_, stop_->get_token(), []() noexcept {}, [](std::exception_ptr) noexcept {})(
         wait(std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay),
              std::function<void()>(std::forward<F>(on_expiry))));
   }

   void cancel()
   {
      if (stop_)
         stop_->request_stop();
      stop_.reset();
   }

private:
   Executor executor_;
   std::optional<std::stop_source> stop_;
};

// =================================================================================================

namespace io
{

template <typename Stream>
auto read_some(Stream& stream, asio::mutable_buffer buffer)
{
   return stream.read_some(capy::mutable_buffer(buffer.data(), buffer.size()));
}

template <typename Stream, typename ConstBufferSequence>
auto write(Stream& stream, const ConstBufferSequence& buffers)
{
   return capy::write(stream, capy::from_asio(buffers));
}

/// Receives a datagram into \p buffer, on a connected datagram socket. A coroutine, unlike
/// read_some(): corosio's datagram operations do not copy their buffer sequence, as its stream
/// operations do, but point to it, so it has to outlive the operation.
template <typename Socket>
Task<std::tuple<error_code, size_t>> receive(Socket& socket, asio::mutable_buffer buffer)
{
   const capy::mutable_buffer buffers(buffer.data(), buffer.size());
   auto [ec, n] = co_await socket.recv(buffers);
   co_return std::tuple{ec, n};
}

/// Waits until \p socket has something to read, for a caller that reads it by hand.
template <typename Socket>
auto wait_readable(Socket& socket)
{
   return socket.wait(corosio::wait_type::read);
}

/// Peeks at what has arrived on \p socket, without taking it. corosio has no flags for its
/// receive operations, so this waits for readiness and peeks at the native socket.
template <typename Socket>
Task<std::tuple<error_code, size_t>> peek(Socket& socket, asio::mutable_buffer buffer)
{
   for (;;)
   {
      if (auto [ec] = co_await socket.wait(corosio::wait_type::read); ec)
         co_return std::tuple{ec, size_t{0}};

      auto n = ::recv(socket.native_handle(), buffer.data(), buffer.size(), MSG_PEEK);
      if (n > 0)
         co_return std::tuple{error_code{}, static_cast<size_t>(n)};
      if (n == 0)
         co_return std::tuple{errors::eof, size_t{0}};
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
         co_return std::tuple{last_error(), size_t{0}};
   }
}

} // namespace io

// =================================================================================================

} // namespace anyhttp
