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
#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>
#endif

#include <boost/beast/http/error.hpp>

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

} // namespace anyhttp
