#pragma once

//
// The I/O runtime anyhttp is built on, and the vocabulary the rest of the code reaches it through:
// Boost.Asio with completion tokens (ANYHTTP_ASIO=1), or capy/corosio with coroutines only
// (ANYHTTP_COROSIO=1). A build tree is one or the other, see docs/corosio-port-plan.md.
//
// Apart from the runtime layer (this header, anyhttp/net.hpp and what they include), only the
// public API front-ends and the test fixtures test ANYHTTP_ASIO or ANYHTTP_COROSIO: the protocol
// backends are written once, against these names. asio/runtime.hpp declares them in these
// sections and documents each, corosio/runtime.hpp follows it:
//
//    types        Task<T>, Executor, IpAddress, TcpEndpoint, UdpEndpoint, error_code,
//                 results<Ts...>, IoTask<Ts...>; asio:: for the buffers, which stay Asio's
//    errors       errc, errors::, system_error, throw_error(), last_error(), and ::code() of an
//                 exception_ptr; multithreaded_runtime
//    completions  Completion<Signature>, complete_immediately(), complete(), on_cancel(),
//                 and to and from coroutines: initiate<Signature>(), launch()
//    scheduling   yield_now(), run_later(), dispatch_to(), new_strand(), delay(), when_both(),
//                 when_either(), reset_cancellation(), shielded()
//    engines      Event, Timer
//    I/O          io::read_some(), io::write(), io::peek(), io::receive(), io::wait_readable()
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
