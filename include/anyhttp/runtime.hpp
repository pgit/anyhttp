#pragma once

//
// The I/O runtime anyhttp is built on, and the vocabulary the rest of the code reaches it through:
// Boost.Asio with completion tokens (ANYHTTP_ASIO=1), or capy/corosio with coroutines only
// (ANYHTTP_COROSIO=1). A build tree is one or the other, see docs/corosio-port-plan.md.
//
// Apart from the runtime layer (this header, anyhttp/net.hpp and what they include), only the
// public API front-ends and the test fixtures test ANYHTTP_ASIO or ANYHTTP_COROSIO: the protocol
// backends are written once, against these names.
//
//    Task<T>, Executor, error_code, Completion<Signature>, system_error
//    errc (portable error conditions), errors:: (the codes of the API contract), throw_error()
//    last_error()                                              -- errno, as an error_code
//    complete_immediately(), complete_later(), on_cancel()     -- parked operations
//    initiate<Signature>(), launch()                           -- the two directions between them
//    yield_now(), run_later(), dispatch_to(), new_strand(), delay(), when_both(), when_either()
//    reset_cancellation(), shielded()
//    Event, Timer
//    io::read_some(), io::write(), io::peek(), io::receive(), io::wait_readable()
//
// asio/runtime.hpp has the documentation of each.
//

#include "anyhttp/config.hpp"

#if ANYHTTP_ASIO == ANYHTTP_COROSIO
#error "exactly one of ANYHTTP_ASIO and ANYHTTP_COROSIO has to be 1"
#endif

#if ANYHTTP_COROSIO
#include "anyhttp/corosio/runtime.hpp"
#else
#include "anyhttp/asio/runtime.hpp"
#endif
