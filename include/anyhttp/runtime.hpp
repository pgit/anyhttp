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
#include <boost/system/error_code.hpp>
#endif

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

} // namespace anyhttp
