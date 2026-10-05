#pragma once

//
// The runtime layer for Boost.Asio: completion tokens, asio::awaitable, any_io_executor. See
// anyhttp/runtime.hpp for what this is, and corosio/runtime.hpp for the other runtime, which
// defines the same names.
//

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/multiple_exceptions.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <boost/beast/http/error.hpp>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <exception>
#include <limits>
#include <tuple>
#include <utility>

/// The error code of what \p ptr holds, as thrown by the runtime.
inline boost::system::error_code code(const std::exception_ptr& ptr)
{
   if (!ptr)
      return {};
   try
   {
      std::rethrow_exception(ptr);
   }
   catch (boost::asio::multiple_exceptions& mex)
   {
      return code(mex.first_exception());
   }
   catch (boost::system::system_error& ex)
   {
      return ex.code();
   }
}

namespace anyhttp
{
namespace asio = boost::asio;

/// A coroutine, as the runtime runs it.
template <typename T = void>
using Task = asio::awaitable<T>;

/// What sessions, streams and their readers and writers run on.
using Executor = asio::any_io_executor;

using boost::system::error_code;

// =================================================================================================

/**
 * Portable error conditions. `ec == errc::broken_pipe` compares by condition, so it matches
 * whatever category the runtime reported the error in. `make_error_code(errc::broken_pipe)`,
 * called unqualified and found through ADL, makes a code of the generic category.
 */
namespace errc = boost::system::errc;

/**
 * The error codes anyhttp reports where the two runtimes spell them differently. Most of them
 * are part of the API contract (README.md, "The End of a Body" and "Concurrent Requests"). Every
 * other error is made with `make_error_code(errc::...)`.
 */
namespace errors
{
/// The end of an incoming body.
inline const error_code eof = asio::error::eof;

/// An incoming body that ended before it was complete: a reset stream, a lost connection.
inline const error_code partial_message = boost::beast::http::error::partial_message;

/// An operation that was cancelled before it completed.
inline const error_code canceled = make_error_code(errc::operation_canceled);

/// HTTP/1.1: a request or response that has to wait for an earlier one, see Session.
inline const error_code would_block = asio::error::would_block;

/// A connection that can not carry any more requests or responses.
inline const error_code connection_aborted = asio::error::connection_aborted;

/// An operation on a Reader or Writer after its implementation has been released.
inline const error_code bad_descriptor = asio::error::bad_descriptor;

/// An operation that may be in progress only once at a time.
inline const error_code already_started = asio::error::already_started;

/// A header section larger than Config::max_header_size.
inline const error_code header_limit = boost::beast::http::error::header_limit;
} // namespace errors

/// Whether the runtime supports running a server or client on several threads (with strands).
inline constexpr bool multithreaded_runtime = true;

/// What the runtime throws an error_code as.
using system_error = boost::system::system_error;

/// Throws \p ec, for code that reports errors as exceptions, as the runtime's own does.
[[noreturn]] inline void throw_error(const error_code& ec)
{
   throw boost::system::system_error(ec);
}

/// What `errno` holds, for code that calls the operating system itself.
inline error_code last_error() noexcept { return {errno, boost::system::system_category()}; }

// =================================================================================================

/**
 * An operation that has been started and not completed yet, waiting to be completed with a
 * \p Signature of <tt>void(error_code, ...)</tt>. The protocol backends park these and complete
 * them from their engines' callbacks.
 */
template <typename Signature>
using Completion = asio::any_completion_handler<Signature>;

//
// Completing a parked operation. Invoking a Completion directly resumes its caller right there,
// inside whatever called us, which is fine from a loop of our own, but not from an engine
// callback or from inside the initiating function. The two functions below are for those cases.
//

/**
 * Completes \p handler without doing any I/O, through its associated immediate executor (with
 * \p fallback standing in when the handler has none). This is the one way an operation that has
 * nothing asynchronous left to do may finish: invoking the handler straight from the initiating
 * function would surprise callers that rely on the ASIO guarantee of not being re-entered.
 *
 * A handler that is empty (an \c any_completion_handler detached by cancellation) is quietly
 * dropped -- there is nobody left to tell.
 */
template <typename Handler, typename... Args>
inline void complete_immediately(Handler&& handler, const Executor& fallback, Args&&... args)
{
   if (!handler)
      return;

   asio::any_completion_executor ex = asio::get_associated_immediate_executor(handler, fallback);
   ex.execute([handler = std::forward<Handler>(handler),
               ... args = std::forward<Args>(args)]() mutable { //
      std::move(handler)(std::move(args)...);
   });
}

/**
 * Completes \p handler from \p executor's queue, after whatever is running now has returned. This
 * is how an engine callback (or a cancellation handler) completes an operation: resuming the
 * application inside nghttp2 or ngtcp2 would let it call back into the engine from inside it.
 */
template <typename Handler, typename... Args>
inline void complete_later(Handler&& handler, const Executor& executor, Args&&... args)
{
   asio::post(executor, [handler = std::forward<Handler>(handler),
                         ... args = std::forward<Args>(args)]() mutable { //
      std::move(handler)(std::move(args)...);
   });
}

/**
 * Calls \p on_cancel when the caller of the operation \p handler stands for cancels it. The
 * callback runs on the caller's side, which may be another thread with a strand, so all it may do
 * is take the parked handler back and complete it with \ref complete_later. Does nothing if the
 * caller has no way of cancelling.
 */
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto slot = asio::get_associated_cancellation_slot(handler);
   if (slot.is_connected() && !slot.has_handler())
      slot.assign([on_cancel = std::forward<F>(on_cancel)](asio::cancellation_type_t) mutable { //
         on_cancel();
      });
}

// -------------------------------------------------------------------------------------------------

/**
 * Starts an operation that completes a <tt>Completion<Signature></tt>, and waits for it, yielding
 * the completion's arguments as a tuple:
 *
 * \code
 * auto [ec, n] = co_await initiate<ReadSome>([&](Completion<ReadSome> handler) {
 *    impl.async_read_some(buffer, std::move(handler));
 * });
 * \endcode
 *
 * The initiating function runs when the operation is awaited, so it may capture by reference.
 */
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return asio::async_initiate<const asio::as_tuple_t<asio::deferred_t>&, Signature>(
      [init = std::forward<Init>(init)](Completion<Signature> handler) mutable {
         init(std::move(handler));
      },
      asio::as_tuple(asio::deferred));
}

/// Starts \p task on \p executor, detached: nobody waits for it, and what it throws is dropped.
inline void launch(const Executor& executor, Task<void> task)
{
   asio::co_spawn(executor, std::move(task), asio::detached);
}

/// Starts \p task on \p executor and calls \p on_done with what it threw, if anything, when it
/// is done: <tt>void(const std::exception_ptr&)</tt>.
template <typename F>
inline void launch(const Executor& executor, Task<void> task, F&& on_done)
{
   asio::co_spawn(executor, std::move(task), std::forward<F>(on_done));
}

/**
 * Runs \p task on \p executor as the operation \p handler stands for: the task's result
 * completes that operation, and cancelling it cancels the task. A task that throws completes it
 * with the error code of what it threw.
 *
 * This is how an operation that is a coroutine inside is offered with a completion token. The
 * other runtime has no tokens, and returns such a task's result directly.
 */
template <typename... T>
inline void launch(const Executor& executor, Task<std::tuple<error_code, T...>> task,
                   Completion<void(error_code, T...)>&& handler)
{
   auto slot = asio::get_associated_cancellation_slot(handler);
   auto handler_executor = asio::get_associated_executor(handler, executor);

   //
   // co_spawn() gives the task a cancellation slot of its own, so binding the caller's to it is
   // what makes cancelling the operation reach whatever the task is waiting for.
   //
   asio::co_spawn(executor, std::move(task),
                  asio::bind_cancellation_slot(
                     slot, asio::bind_executor(
                              handler_executor, [handler = std::move(handler)](
                                                   const std::exception_ptr& ep,
                                                   std::tuple<error_code, T...> result) mutable {
                                 if (ep)
                                    std::move(handler)(code(ep), T{}...);
                                 else
                                    std::apply(std::move(handler), std::move(result));
                              })));
}

// -------------------------------------------------------------------------------------------------

/// Lets whatever else is ready to run on the caller's executor run first.
inline auto yield_now() { return asio::post(asio::deferred); }

/// Calls \p function from \p executor's queue, after whatever is running now has returned.
template <typename F>
inline void run_later(const Executor& executor, F&& function)
{
   asio::post(executor, std::forward<F>(function));
}

/// Calls \p function on \p executor: right away if the caller runs on it already, later if not.
template <typename F>
inline void dispatch_to(const Executor& executor, F&& function)
{
   asio::dispatch(executor, std::forward<F>(function));
}

/// A new strand on \p executor, for a connection that has to be serialized against itself
/// while the io_context runs on several threads (\c server::Config::use_strand).
inline Executor new_strand(const Executor& executor) { return asio::make_strand(executor); }

/// Waits for \p duration to pass. Cancelling the wait completes it early, with the error code
/// the runtime's timers report for that.
inline Task<error_code> delay(std::chrono::steady_clock::duration duration)
{
   asio::steady_timer timer(co_await asio::this_coro::executor);
   timer.expires_after(duration);
   auto [ec] = co_await timer.async_wait(asio::as_tuple);
   co_return ec;
}

/**
 * Runs \p a and \p b concurrently, until both are done, and yields what they return: nothing,
 * the one value, or both as a tuple. If one of them throws, the other is cancelled, and the
 * exception is rethrown once both are done.
 */
template <typename A, typename B>
auto when_both(Task<A> a, Task<B> b)
{
   using namespace asio::experimental::awaitable_operators;
   return std::move(a) && std::move(b);
}

/**
 * Runs \p a and \p b concurrently, until one of them succeeds, which cancels the other, and yields
 * what it returned as a variant, with \c std::monostate for nothing. One that throws does not
 * succeed: only when both have thrown is one of the exceptions rethrown (wrapped in ASIO's
 * multiple_exceptions). Waits for the other one to finish all the same.
 */
template <typename A, typename B>
auto when_either(Task<A> a, Task<B> b)
{
   using namespace asio::experimental::awaitable_operators;
   return std::move(a) || std::move(b);
}

/**
 * Lets a coroutine that has been cancelled go on awaiting, to clean up after the cancellation:
 * ASIO throws from everything it awaits after that otherwise -- a nested coroutine included, which
 * is why this has to be awaited in the cancelled coroutine itself. In COROSIO, there is nothing to
 * reset, see shielded().
 */
inline auto reset_cancellation() { return asio::this_coro::reset_cancellation_state(); }

/**
 * Awaits \p task although the caller has been cancelled already, once reset_cancellation() has
 * been awaited. With ASIO, that is all it takes. A stop request in COROSIO can not be taken back,
 * and there, the task runs with a stop token of its own instead.
 */
template <typename T>
Task<T> shielded(Task<T> task)
{
   return task;
}

// =================================================================================================

/**
 * Wakes up the coroutine that waits for it, as capy's \c async_event does. The event is latched:
 * one that is set while nobody waits is still set when the next \c wait() comes along, which then
 * does not wait at all. A loop that waits for more work clears it at its top, before it looks for
 * work, so that nothing set while it was looking gets lost.
 *
 * \c set() never resumes the waiting coroutine itself, but posts that to the coroutine's
 * executor: it is called from inside engine callbacks. Only one coroutine may wait at a time, and
 * all calls come from the strand that coroutine runs on.
 */
class Event
{
public:
   Event() = default;
   Event(const Event&) = delete;
   Event& operator=(const Event&) = delete;
   ~Event()
   {
      if (waiter_)
         wake(errors::canceled);
   }

   void set()
   {
      set_ = true;
      if (waiter_)
         wake(error_code{});
   }

   void clear() noexcept { set_ = false; }
   bool is_set() const noexcept { return set_; }

   /**
    * Waits for the event to be set: <tt>auto [ec] = co_await event.wait();</tt>. The wait can be
    * cancelled, and then completes with \c errors::canceled.
    */
   Task<std::tuple<error_code>> wait()
   {
      if (set_)
         co_return error_code{};

      executor_ = co_await asio::this_coro::executor;
      co_return co_await asio::async_initiate<const asio::as_tuple_t<asio::deferred_t>&,
                                              void(error_code)>(
         [this](Completion<void(error_code)> waiter) {
            assert(!waiter_);
            waiter_ = std::move(waiter);
            on_cancel(waiter_, [this] {
               if (waiter_)
                  wake(errors::canceled);
            });
         },
         asio::as_tuple(asio::deferred));
   }

private:
   void wake(error_code ec) { complete_later(std::move(waiter_), executor_, ec); }

   bool set_ = false;
   Completion<void(error_code)> waiter_;
   Executor executor_; // of the coroutine waiting in waiter_
};

// -------------------------------------------------------------------------------------------------

/**
 * A timer that calls back when it expires, which is what an engine with timeouts of its own
 * (ngtcp2) needs: it tells when, and wants to be called then. Arming the timer again replaces
 * both the expiry and the callback, and a callback that has been replaced or cancelled is never
 * called. Callbacks run on the timer's executor.
 */
class Timer
{
public:
   explicit Timer(const Executor& executor) : timer_(executor) {}

   template <typename Rep, typename Period, typename F>
   void arm(std::chrono::duration<Rep, Period> delay, F&& on_expiry)
   {
      timer_.expires_after(delay);
      timer_.async_wait([on_expiry = std::forward<F>(on_expiry)](const error_code& ec) mutable {
         if (!ec)
            on_expiry();
      });
   }

   void cancel() { timer_.cancel(); }

private:
   asio::steady_timer timer_;
};

// =================================================================================================

//
// I/O on the streams the sessions run on, yielding a tuple in both runtimes:
//
//    auto [ec, n] = co_await io::read_some(stream, buffer);
//
// Always call these qualified: unqualified, ADL would find boost::asio's synchronous read_some()
// and write() for an ASIO stream as well.
//
namespace io
{

/// Reads some bytes into \p buffer: <tt>(error_code, size_t)</tt>.
template <typename Stream>
auto read_some(Stream& stream, asio::mutable_buffer buffer)
{
   return stream.async_read_some(buffer, asio::as_tuple);
}

/// Writes all of \p buffers, unless an error comes first: <tt>(error_code, size_t)</tt>.
template <typename Stream, typename ConstBufferSequence>
auto write(Stream& stream, const ConstBufferSequence& buffers)
{
   //
   // As few writes as the stream takes: async_write()'s default completion condition, transfer_all,
   // would split them into 64 KiB each. A large HTTP/1.1 header then takes several round trips
   // through the reactor, and a server that rejects it early catches the client in the middle.
   //
   return asio::async_write(
      stream, buffers,
      [](const error_code& ec, size_t) -> size_t {
         return ec ? 0 : std::numeric_limits<size_t>::max();
      },
      asio::as_tuple);
}

/// Peeks at what has arrived on \p socket, without taking it: <tt>(error_code, size_t)</tt>.
template <typename Socket>
auto peek(Socket& socket, asio::mutable_buffer buffer)
{
   return socket.async_receive(buffer, Socket::message_peek, asio::as_tuple);
}

/// Receives a datagram into \p buffer, on a connected datagram socket: <tt>(error_code,
/// size_t)</tt>.
template <typename Socket>
auto receive(Socket& socket, asio::mutable_buffer buffer)
{
   return socket.async_receive(buffer, asio::as_tuple);
}

/// Waits until \p socket has something to read, for a caller that reads it by hand
/// (<tt>recvmsg()</tt> on its native handle): <tt>(error_code)</tt>.
template <typename Socket>
auto wait_readable(Socket& socket)
{
   return socket.async_wait(Socket::wait_read, asio::as_tuple);
}

} // namespace io

// =================================================================================================

} // namespace anyhttp