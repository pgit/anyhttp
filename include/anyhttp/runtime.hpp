#pragma once

//
// The I/O runtime anyhttp is built on, and the vocabulary the rest of the code reaches it through:
// Boost.Asio with completion tokens (ANYHTTP_CAPY=0), or capy/corosio with coroutines only
// (ANYHTTP_CAPY=1). A build tree is one or the other, see docs/capy-port-plan.md.
//
// Apart from the runtime layer (this header, anyhttp/net.hpp and what they include), only the
// public API front-ends and the test fixtures test ANYHTTP_CAPY: the protocol backends are written
// once, against these names.
//
//    Task<T>, Executor, error_code, Completion<Signature>, system_error
//    errc (portable error conditions), errors:: (the codes of the API contract), throw_error()
//    last_error()                                              -- errno, as an error_code
//    complete_immediately(), complete_later(), on_cancel()     -- parked operations
//    initiate<Signature>(), launch()                           -- the two directions between them
//    run_later(), dispatch_to(), new_strand(), delay(), yield_now(), when_both(), when_either()
//    reset_cancellation(), shielded()
//    Event, Timer
//    io::read_some(), io::write(), io::peek(), io::receive(), io::wait_readable()
//
// detail/runtime_asio.hpp has the documentation of each.
//

#ifndef ANYHTTP_CAPY
#define ANYHTTP_CAPY 0
#endif

#if ANYHTTP_CAPY
#include "anyhttp/detail/runtime_capy.hpp"
#else
#include "anyhttp/detail/runtime_asio.hpp"
#endif
