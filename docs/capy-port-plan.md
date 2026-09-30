# Porting anyhttp to capy/corosio, next to ASIO

The goal: one source tree that builds either as today, on Boost.Asio with completion tokens, or on
capy/corosio with a coroutine-only API. The choice is made at configure time. See
[capy-corosio-lessons.md](capy-corosio-lessons.md) for the recipes this builds on; this document
is about what is specific to keeping **both**.

---------------------------------------------------------------------------------------------------

## 0. Evidence so far (spike, 2026-09-30)

A standalone spike (capy `a372a6b`, corosio `6a3eed4`, both `develop`) established:

- capy and corosio build with this tree's toolchain: clang 23, libc++ and the Boost 1.92 in
  `/opt/libc++`. liburing 2.9 is present, so corosio enables its io_uring backend.
- Boost.Asio's buffer headers, Beast's sans-I/O `http::parser`/`http::serializer` and corosio's
  sockets coexist in one TU. An HTTP/1.1 request/response over `corosio::tcp_socket`, with Beast
  driven by `put()`/`next()`/`consume()`, works. This is the missing evidence for lessons §9.2,
  "HTTP/1.1 (Beast)".
- **corosio's `openssl_stream` does not compile against AWS-LC.** `src/openssl/src/detail/engine.cpp`
  uses `BIO_nwrite0`/`BIO_nwrite`, which BoringSSL and AWS-LC removed. It builds and links against
  the system OpenSSL 3.5.7.

---------------------------------------------------------------------------------------------------

## 1. Shape of the dual build

- **One switch per build tree**: `-DANYHTTP_API=ASIO|CAPY` (default ASIO) defines
  `ANYHTTP_CAPY=0|1`. A binary is one or the other, never both. The CAPY tree is `build-capy/`.
- **CAPY means corosio all the way down**: its reactor, sockets, timers and TLS; `capy::task`;
  results as `io_result` tuples. Boost.Asio *headers* stay, because Beast needs asio's buffer
  and error types (capy's `buffers/asio.hpp` bridges the buffers). No `io_context`, socket,
  handler or `awaitable` of ASIO's is used.
- **CAPY requires `TLS_LIBRARY=OpenSSL`** (see §0). The "one TLS library per process" rule holds:
  corosio's `openssl_stream` and `ngtcp2_crypto_ossl` both use OpenSSL 3.5. This constraint goes
  away only if corosio's engine stops using the BIO pair's zero-copy API (a candidate upstream PR).
- **Single source for the protocol code.** The `#if`s are confined to three places, the way the
  TLS `#if`s are confined today:
  1. an internal runtime layer (§3),
  2. the public API front-ends (`reader.hpp`, `writer.hpp`, `session.hpp`, `client.hpp`,
     `server.hpp`),
  3. the test fixtures.

  The `h1_`/`h2_`/`h3_` backends compile unchanged in both modes. A second implementation of
  each backend would double the maintenance of the part with the most protocol knowledge in it.
- **Pin capy and corosio to commit SHAs** in FetchContent, never `develop` (lessons §2 and §6: the
  sibling project's history stopped building because of drift).

---------------------------------------------------------------------------------------------------

## 2. Where the mapping is hard

Ranked by how much each one threatens the approach, the most threatening first.

| # | Area | ASIO today | capy/corosio | Approach |
|---|---|---|---|---|
| 1 | **Parked operations and their cancellation** | Backends park an `any_completion_handler<Sig>` (read, write, get_response) and complete it from engine callbacks. Cancellation is per operation, through its cancellation slot (terminal/partial/total), and is what `cancel_after` drives. | A parked `coroutine_handle` plus the `io_env` from `await_suspend`. Cancellation comes from the *chain's* `std::stop_token`, which a `std::stop_callback` observes and which may fire on any thread. There is no per-operation slot: per-operation cancellation means `when_any` or `corosio::timeout` around the one operation. | A `Completion<Sig>` type: `any_completion_handler` in ASIO, a continuation + env + result slot + `stop_callback` in CAPY. It offers `complete(...)`, `complete_immediately(...)` and `on_cancel(fn)`. The backends' `Reader::Impl`/`Writer::Impl` virtuals take it in place of today's handler aliases. Stop maps to *terminal* cancellation, which is what the README's `async_write_eof()` rules are written for. |
| 2 | **Completion resumes inline vs. posted** | `swap_and_invoke()` resumes the waiting coroutine inline, from inside nghttp2/nghttp3 callbacks. The recursion guards (`inside_call_read_handler_`) exist because of that. | Lessons §3.2: never resume application code inside an engine callback; `async_event::set()` posts. | In CAPY, `Completion` posts to `env->executor`. The guards stay; they just never trigger there. Note that ASIO's `co_spawn` *also* starts inline (it `dispatch`es), so "spawn starts inline" (lessons §4.4) is not a new hazard. |
| 3 | **Error vocabulary** | `boost::system::error_code`. The README contract is `asio::error::eof`, `http::error::partial_message`, `asio::error::would_block`/`connection_aborted`, `errc::broken_pipe`. | `std::error_code`; `capy::error::eof`, `capy::error::stream_truncated`, compared through `capy::cond::*`. | An `anyhttp::error_code` alias plus named constants per mode (`errors::eof`, `errors::truncated`, ...). Beast's own codes (`header_limit`, parser errors) cross over through Boost.System's `std::error_code` interop. Needs a test that compares through the conversion. The README gets a per-mode table. |
| 4 | **HTTP/1.1** | `h1_session.cpp` (1.4k lines) uses `http::async_read`/`async_write`, which need an ASIO AsyncStream. | No AsyncStream exists. | Move **both** modes onto Beast's parser/serializer, driven by read/write loops on the runtime layer (spike-proven, §0). This is the one step that changes ASIO behaviour-bearing code substantially, so it comes before any capy work, with the full suite green. |
| 5 | **TLS over TCP, detection** | `asio::ssl::stream`. `detect_ssl`/`detect_h2` sniff into a `flat_buffer`, which is handed to the SSL handshake or the session. | `openssl_stream` + `tls_context` (`set_alpn`, `alpn_protocol()`). Its handshake takes no pre-read bytes, and it does not build against AWS-LC. | Detection becomes a portable prefix sniff. In CAPY, a `PrefixedStream<S>` replays the sniffed bytes beneath `openssl_stream` (lessons §9.2). Sessions already accept a pre-read `Buffer`. Spike the TLS-over-prefix stack first. |
| 6 | **Timers** | `steady_timer`, re-armed for ngtcp2's expiry and elsewhere. | Public API: `corosio::delay` and `corosio::timeout`. A re-armable `timer` exists, but only as `corosio::detail::timer`. | The runtime layer offers a `Timer` with `expires_at`, `wait` and `cancel`. Build the CAPY one on `detail::timer`, with a regression test, or restructure the h3 expiry loop the way lessons §3.1 does (`timeout(event.wait(), deadline)`). Decide at the h3 step. |
| 7 | **Threads and strands** | `use_strand`, a per-connection strand, `server_main --threads`. Tests are single-threaded (`MULTITHREADED` is off). | `capy::strand` exists and corosio's `io_context` is thread-safe by default. But `async_event` is single-threaded by design, and nothing in the sibling project ran multi-threaded. | CAPY starts single-threaded, and `use_strand` fails loudly there. Threads come last, if ever (lessons §9.2). |
| 8 | **Composite operations** | `awaitable_operators` `&&`/`\|\|` (send_loop && recv_loop, and 33 uses in tests). `&&` cancels its sibling only on an exception. | `when_all`/`when_any`. `when_all` also requests stop on the first `io_result` *error*, and `corosio::timeout` loses the progress of a composite operation. | `when_both`/`when_either` helpers in the runtime layer. The loops return `task<void>`, so `when_all`'s error-stop never applies to them. |
| 9 | **Executors and lifetimes** | `any_io_executor` copies, kept even by detached readers and writers so they can still complete. `bind_executor` makes `cancel_after` find one. | `executor_ref` is non-owning; `capy::any_executor` owns. Destroying an `io_context` with a frame still suspended on it double-frees (lessons §4.2). | Store `any_executor`. CAPY fixtures drain the context before destroying it; nothing may rely on "destroy the context and let it clean up". |
| 10 | **Public types that leak ASIO** | `local_endpoint()` returns `asio::ip::tcp::endpoint`. Buffers are `asio::mutable_buffer`. `executor_type`. The `RequestHandler` returns `awaitable<void>`. | `corosio::endpoint`, capy buffer sequences, `task<void>`. | Per-mode aliases in `common.hpp`. `Fields` (Beast) and `boost::url` stay the same in both. |
| 11 | **Tests** | 5.5k lines written in ASIO idioms: 63 `as_tuple`, 20 `cancel_after`, 19 `co_spawn`, 33 awaitable operators. The `External` fixture drives Boost.Process on ASIO. | Different spelling everywhere. Boost.Process needs its own ASIO context and thread (lessons §4.5). | Open decision (§5). |

Not problems, checked:

- The engines themselves: nghttp2, ngtcp2 and nghttp3 are sans-I/O.
- The UDP path: corosio's `udp_socket::wait(wait_type::read)` plus `native_handle()` is exactly
  how h3 already does GSO/GRO by hand.
- Resolving: `corosio::resolver`.
- Logging, formatters, `alt_svc`, the file handler.
- The h2/h3 include boundary, which carries over unchanged. Add one rule next to it: only the
  runtime layer includes ASIO or corosio I/O headers.

---------------------------------------------------------------------------------------------------

## 3. The runtime layer (sketch)

`include/anyhttp/detail/runtime.hpp`, plus one implementation header per mode:

```cpp
namespace anyhttp::rt {
template <typename T = void> using Task = /* asio::awaitable<T> | capy::task<T> */;
using Executor   = /* asio::any_io_executor | capy::any_executor */;
using error_code = /* boost::system::error_code | std::error_code */;
namespace errors { /* eof, truncated, would_block, connection_aborted, broken_pipe, ... */ }

template <typename Sig> class Completion;   // §2 #1: complete / complete_immediately / on_cancel
void spawn(Executor, Task<void>);           // detached; logs what escapes

// I/O as awaitables yielding tuples in both modes: auto [ec, n] = co_await rt::read_some(s, b);
auto read_some(auto& stream, asio::mutable_buffer);
auto write(auto& stream, auto const& buffers);
auto wait_readable(auto& udp_socket);
class Timer;                                // §2 #6
Task<void> when_both(Task<void>, Task<void>);
}
```

In ASIO mode each helper is a one-line forward (`s.async_read_some(b, as_tuple(use_awaitable))`),
so it adds no coroutine frame, and ASIO behaviour does not change. In CAPY mode the public front
ends become awaitables whose `await_suspend` builds a `Completion` and calls the same `*_any`
entry point the token front ends call today. That is the pattern of the old `capy` branch, with
corosio underneath instead of ASIO.

---------------------------------------------------------------------------------------------------

## 4. Steps

Each step ends with the ASIO suite green, both serial and under `gtest-parallel`. Steps that touch
the h3 write path also run under ASAN. From step 5 on, CAPY's own tests must pass as well.

1. **Build plumbing.** `ANYHTTP_API`, pinned FetchContent, the `build-capy/` tree, and CAPY
   requiring OpenSSL. `build-capy` builds a smoke test only, and the ASIO build is untouched.
   *Done 2026-09-30.* `test/test_corosio.cpp` holds the spike's findings as tests: Beast's
   parser/serializer over corosio, and why each mode needs its own error constants.
2. **Runtime layer, ASIO half; move the internals onto it.**
   - 2a: vocabulary (`Task`, `Executor`, errors, `Completion`, `spawn`). This is mechanical.
   - 2b: h2 loops and streams.
   - 2c: h3.
   - 2d: `server_impl`, `client_impl`, `session`.

   *2a–2c done 2026-09-30* (`fa8b4d5`..`63d0894`). `runtime.hpp` has `Task`, `Executor`,
   `error_code`, `Completion`, `errc`/`errors`, `complete_immediately`/`complete_later`,
   `on_cancel`, `run_later`, `dispatch_to`, `new_strand`, `launch`, `when_both`, `Event`, `Timer`,
   and `io::read_some`/`write`/`receive`/`wait_readable`. Two findings:
   - `Event::set()` posts in ASIO too, as capy's does, because h3 signals from inside ngtcp2.
     This made small h2 GETs 19% faster, since `start_write()`s now coalesce.
   - The h2 receive loop stopped only because `start_write()` used to drain the GOAWAY inline. The
     send loop now cancels the pending read when it ends.
3. **HTTP/1.1 on Beast's parser and serializer**, in ASIO mode. This has the largest
   behavioural risk: h2c upgrade, chunked bodies, 431, the "max concurrent streams = 1" rules.
4. **Detection and stream plumbing, portable.** Prefix sniffing, `PrefixedStream`, a TLS stream
   alias. `any_async_stream` becomes ASIO-only or retires. Spike `openssl_stream` over
   `PrefixedStream` first, including full duplex (corosio#330/#331).
5. **CAPY half of the runtime layer, plus the public front ends.** Bring it up one protocol at a
   time, each with its slice of tests in `build-capy`:
   - 5a: h2c with prior knowledge
   - 5b: HTTP/1.1 and h2c upgrade
   - 5c: TLS with ALPN
   - 5d: HTTP/3
6. **The test suite in both modes** (§5).
7. **README**: the two API styles, the per-mode error table, the constraints (OpenSSL, single
   thread).
8. Later: threads in CAPY; an upstream fix for `openssl_stream` on AWS-LC.

Out of scope for the port, and kept as separate commits if wanted: what lessons §9.1 lists as
"what this project does better" in h3 (parking on `EAGAIN`, `write_aggregate_pkt`, and so on). A
port that also changes behaviour cannot be verified against the old tests.

---------------------------------------------------------------------------------------------------

## 5. Decisions (2026-09-30)

- **Single source** (§1). The ASIO internals move onto the runtime layer, HTTP/1.1 onto Beast's
  parser/serializer, and all of it happens before any capy code runs.
- **The CAPY API is capy-idiomatic**: `read_some`, `write`, `write_eof`, `submit`,
  `get_response`, `connect`, `get`. `Reader` and `Writer` model
  `capy::ReadStream`/`capy::WriteStream`.
- **Tests are shared, plus per mode.** Protocol-behaviour tests are written once, against thin
  per-mode helpers in the fixtures. Token and `cancel_after` tests stay ASIO-only; `stop_token`
  and `timeout` tests are CAPY-only. While the port is under way, `test/CMakeLists.txt` lists
  the files CAPY builds explicitly. That list grows until it equals the glob.
