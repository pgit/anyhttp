#pragma once

//
// The I/O runtime anyhttp is built on, and the vocabulary the rest of the code reaches it through:
// Boost.Asio with completion tokens (ANYHTTP_CAPY=0), or capy/corosio with coroutines only
// (ANYHTTP_CAPY=1). A build tree is one or the other, see docs/capy-port-plan.md.
//
// Apart from this header, only the public API front-ends and the test fixtures test ANYHTTP_CAPY:
// the protocol backends are written once, against the names defined here.
//

#ifndef ANYHTTP_CAPY
#define ANYHTTP_CAPY 0
#endif

#if ANYHTTP_CAPY
#error "the CAPY runtime is not implemented yet, see docs/capy-port-plan.md"
#else
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>
#endif

#include <boost/beast/http/error.hpp>

#include <cassert>
#include <utility>

namespace anyhttp
{
namespace asio = boost::asio;

// =================================================================================================

/// A coroutine, as the runtime runs it.
template <typename T = void>
using Task = asio::awaitable<T>;

/// What sessions, streams and their readers and writers run on.
using Executor = asio::any_io_executor;

using boost::system::error_code;

/**
 * An operation that has been started and not completed yet, waiting to be completed with a
 * \p Signature of <tt>void(error_code, ...)</tt>. The protocol backends park these and complete
 * them from their engines' callbacks.
 */
template <typename Signature>
using Completion = asio::any_completion_handler<Signature>;

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

// =================================================================================================

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

/// Calls \p function from \p executor's queue, after whatever is running now has returned.
template <typename F>
inline void run_later(const Executor& executor, F&& function)
{
   asio::post(executor, std::forward<F>(function));
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

/// Runs \p a and \p b concurrently, until both are done. If one of them throws, the other is
/// cancelled, and the exception is rethrown once both are done.
inline Task<void> when_both(Task<void> a, Task<void> b)
{
   using namespace asio::experimental::awaitable_operators;
   co_await (std::move(a) && std::move(b));
}

// =================================================================================================

/**
 * Wakes up the coroutine that waits for it, as capy's \c async_event does. The event is latched:
 * one that is set while nobody waits is still set when the next \c wait() comes along, which then
 * does not wait at all. A loop that waits for more work clears it at its top, before it looks for
 * work, so that nothing set while it was looking gets lost.
 *
 * Only one coroutine may wait at a time, and all calls come from the same strand. For ASIO,
 * \c set() resumes the waiting coroutine right there, inside \c set().
 */
class Event
{
public:
   void set()
   {
      set_ = true;
      if (waiter_)
         std::exchange(waiter_, nullptr)();
   }

   void clear() noexcept { set_ = false; }
   bool is_set() const noexcept { return set_; }

   Task<void> wait()
   {
      if (set_)
         co_return;

      co_await asio::async_initiate<const asio::deferred_t&, void()>(
         [this](Completion<void()> waiter) {
            assert(!waiter_);
            waiter_ = std::move(waiter);
         },
         asio::deferred);
   }

private:
   bool set_ = false;
   Completion<void()> waiter_;
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
   return asio::async_write(stream, buffers, asio::as_tuple);
}

} // namespace io

// =================================================================================================

} // namespace anyhttp
