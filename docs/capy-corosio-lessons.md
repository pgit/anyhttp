# Wrapping sans-I/O protocol engines with capy/corosio

What this project learned about exposing nghttp2, ngtcp2 and nghttp3 through a coroutine-native
capy/corosio API, and the recipes that carry over to porting [anyhttp](https://github.com/pgit/anyhttp)
off ASIO.

The project started out as `nghttp2-corosio`, a re-implementation of nghttp2-asio over HTTP/2 on
TCP. It became `nghttp3-corosio` when HTTP/2 was replaced by HTTP/3 over QUIC (`728376f`, renamed in
`9dbea45`). That makes it one of the few codebases that has driven **both kinds of engine** through
the same coroutine framework: a byte-stream engine (nghttp2 over a `tcp_socket`/TLS stream) and a
datagram engine with its own timers (ngtcp2 over a shared `udp_socket`). anyhttp needs both, and
an HTTP/1.1 engine on top.

Most of the detailed "why" is already written down in `CLAUDE.md` and in the commit messages. This
document pulls it together into lessons and recipes, and says where to find the details. Every
claim is tagged by how well it is backed:

- **Done here:** implemented in this repository and covered by tests, valgrind and/or benchmarks.
- **Done here, HTTP/2:** implemented before the switch to QUIC. The code is in git at `c88c4f4`, the
  last HTTP/2 commit.
- **Not tried:** a recommendation for anyhttp that nothing in this repository has exercised.

---------------------------------------------------------------------------------------------------

## 1. How the project got here

| Phase | Commits | What was learned |
|---|---|---|
| Raw corosio | `a8a94b1` | An accept loop, and a coroutine per connection. |
| nghttp2 over TCP | `e6e3d56`..`d9a3f81` | Send/receive loops around `nghttp2_session_mem_{send,recv}2`, a `Role` enum instead of templates. |
| Bodies as streams | `44c8cb7`, `9416f05` | The `Stream` bridge from synchronous callbacks to `read_some`/`write_some`, and the first real bug: a flood of empty DATA frames. |
| Structured teardown | `18b59ef` | The `TaskGroup` service replaced "leak every `Server` in tests". |
| API refinement | `bd287fd`..`f04cd15` | Explicit `submit()`, `get_response()` racing the body write, capy's stock `ReadStream`/`WriteStream`. |
| TLS over TCP | `8b2f511`, `c96678c` | `corosio::openssl_stream` deadlocked when used full-duplex (corosio#330) and worked after corosio#331. |
| HTTP/3 spike | `ba418d5` | A throwaway target showed corosio's `udp_socket` could carry QUIC, and found four traps before the port began. |
| HTTP/3 port | `728376f`, `0099536` | Demultiplexing, staged writes, split flow control, and giving stream credit back. |
| Datagram features | `85a619d`..`5073dfd`, `e705948`, `b91b02a` | GSO/GRO (+60%), IP_PKTINFO, ECN, Retry, dual-stack. All done by going around corosio. |

The old name was kept until the rewrite had landed, so the protocol diff stayed reviewable. The
rename then got a mechanical commit of its own. The spike was deleted once it had done its job,
and git still has it.

---------------------------------------------------------------------------------------------------

## 2. The mental model: ASIO → capy/corosio

capy provides the coroutine, buffer and executor layer. corosio provides the reactor, sockets,
timers and TLS. There are no completion tokens: everything is a coroutine.

| ASIO / anyhttp | capy / corosio | Notes |
|---|---|---|
| `asio::awaitable<T>` | `capy::task<T>` | |
| operation completing `(ec, n)` | `capy::io_task<T...>` → `io_result`, a `std::tuple<error_code, T...>` | Destructure it: `auto [ec, n] = co_await ...`. No exceptions on the I/O path. |
| `co_spawn(ex, t, handler)` | `capy::run_async(ex, [stop_token], on_done, on_error)(t)` | It starts **inline**, not posted (§4.4). |
| completion tokens, `any_completion_handler` | none | The API becomes coroutine-only. anyhttp's callback and `use_awaitable` flexibility does not carry over. |
| cancellation slots | a `std::stop_token` that flows down the coroutine chain | `co_await capy::this_coro::stop_token` reads it. |
| `steady_timer` | `corosio::delay(d)`, `corosio::timeout(awaitable, d_or_deadline)` | A timeout reports `capy::cond::timeout`, not `canceled`. |
| `&&` / `\|\|` awaitable operators | `capy::when_all` / `capy::when_any` | |
| `async_read` / `async_write` | `capy::read` / `capy::write` | Free algorithms composed over `read_some` / `write_some`. |
| AsyncReadStream / AsyncWriteStream | `capy::ReadStream` / `capy::WriteStream` concepts; `any_read_stream`, `any_write_stream`, `any_stream` | Structural, so no base classes are needed (anyhttp's `impl::Reader`/`Writer`). |
| a channel or condition variable used as "wake me" | `capy::async_event` (`set()`, `clear()`, `wait()`) | `set()` **posts** waiters and never resumes them inline. Much of the design depends on this (§3.2). |
| `asio::strand`, `std::mutex` | nothing | Single-threaded, so nothing is needed. `capy::async_mutex` serializes coroutines, not threads. |
| `io_context::run()` | `corosio::io_context::run/run_one/poll/restart/stop` | `stop()` latches: `restart()` is needed before running again. |
| `asio::error::eof`, `http::error::partial_message` | `capy::error::eof`, `capy::error::stream_truncated` | Compare against `capy::cond::*` conditions. |

**Read the fetched sources, not the docs.** capy and corosio come from `develop` through
FetchContent. Over this project's life they changed return conventions (a struct with `.ec` became
a tuple, `dc8f974`), removed the `any_read_source`/`any_write_sink` concept family (`4dc05fa`),
reshaped the endpoint and socket-family API (`a93a106`), and added `[[nodiscard]]`s. Check
`build/_deps/{capy,corosio}-src` before assuming an API exists. Old commits no longer build against
today's dependencies, and that is drift, not a bug in those commits.

---------------------------------------------------------------------------------------------------

## 3. The core recipe: engine + loops + per-stream bridge

All three libraries are **sans-I/O**: you hand them bytes, they call you back synchronously, and
you ask them for bytes to send. The wrapping has three layers.

### 3.1 Session: one engine, two loops, one event (done here)

```
             ┌──────────── Session::Impl ─────────────┐
socket ─────►│ recv_loop:  read → feed engine ────────┼──► callbacks → Stream objects
             │                  └─► start_write()     │
socket ◄─────│ flush_loop: drain engine → write       │◄── start_write() from anywhere
             │             park on write_ready_       │
             └────────────────────────────────────────┘
```

- **Byte-stream engine (HTTP/2, `c88c4f4:src/session.cpp`).** `run()` is
  `co_await capy::when_all(send_loop(), recv_loop())`. `recv_loop` does `read_some` →
  `nghttp2_session_mem_recv2` → `start_write()`. `send_loop` drains `nghttp2_session_mem_send2`
  into a small accumulator (1460 B), writes it with `capy::write`, and parks on the event once
  nghttp2 has nothing more to give.
- **Datagram engine with timers (HTTP/3, `src/session.cpp`).** The flush loop also has to wake up
  at ngtcp2's own deadline, so it races the event against that deadline:

  ```cpp
  for (;;) {
     write_ready_.clear();                        // at the TOP, before flush() -- see below
     if (auto [ec] = co_await flush(); ec) break;
     auto expiry = ngtcp2_conn_get_expiry2(conn_);
     if (expiry == UINT64_MAX) { co_await write_ready_.wait(); continue; }
     auto [ec] = co_await corosio::timeout(write_ready_.wait(), deadline_from(expiry));
     if (ec == capy::cond::timeout) ngtcp2_conn_handle_expiry(conn_, timestamp());
     else if (ec) break;                          // canceled: structured shutdown
  }
  ```

  `corosio::timeout` covers the whole timer-management job that anyhttp needs a separate
  `steady_timer` and its handlers for. The closing period is likewise just a `corosio::delay(3 *
  PTO)` at the end of `run()`.
- **Clear the event before the operation that can suspend, not right before the wait.** `flush()`
  suspends inside the send. A `start_write()` raised by a callback during that suspension would be
  wiped out by a `clear()` placed later, and the result is a silent hang, not an error.
- **Use a `Role` enum, not templates.** anyhttp templates its nghttp2 session on the stream type,
  so that server and client can share the loops. Here the transport is not a template parameter:
  it is a `capy::any_stream` for TCP/TLS, and a `shared_ptr<udp_socket>` for QUIC (shared by all
  sessions on a server, connected and private on a client). So one class covers both roles. The
  enum decides only which `*_server_new`/`*_client_new` to call and whether `run()` owns a
  receive loop. anyhttp's `h3_session.*`, which is shared across roles,
  already works this way. Extend the approach to `h2_*` and `h1_*`.

### 3.2 The callback rule (done here)

> A callback may call `async_event::set()`, mutate containers, and call into the engine. Nothing
> else. In particular, it must never resume application code inline.

Callbacks run inside `ngtcp2_conn_read_pkt`, `ngtcp2_conn_writev_stream` or
`nghttp2_session_mem_recv2`. If application code runs there and, for example, tears the session
down, it calls back into the engine from inside the engine. ngtcp2 then trips its
`conn->log.last_ts <= ts` assertion. In practice the rule costs nothing, because
`async_event::set()` posts and every wake-up goes through an event. Enforce the narrow rule and you
get the broad one for free.

Two corollaries that each caused a real bug:

- **Copy the `shared_ptr` out of a container before calling into the engine.** A callback can
  erase the very entry you are holding a reference to (for example, retiring a connection ID):
  `auto session = it->second; session->handle_packet(...)`.
- **Keep an object alive across its own erasure** when a callback closes it
  (`Session::Impl::close_stream`).

### 3.3 Stream: a bridge from synchronous callbacks to `read_some`/`write_some` (done here)

One `Stream` per HTTP stream (`src/stream_impl.hpp`), wrapped by `StreamReader`/`StreamWriter`,
which satisfy `capy::ReadStream`/`WriteStream` and are type-erased into the public
`Session::Reader`/`Writer`.

**Read side (protocol-agnostic; survived the HTTP/2→3 switch unchanged):**

- The data callback calls `on_data()`. If a `read_some()` is parked with nothing buffered, the
  bytes are copied **directly into the caller's buffers** through a `ReadSink` pointer that the
  parked coroutine published. Only the overflow goes into a `deque` of chunks. That gained 3-5%
  and avoids the O(n) shift that erasing from the front of a flat vector costs (`9820c50`).
- End-of-body: `eof` if the peer finished cleanly, `stream_truncated` if the stream closed or was
  reset without that. Check `read_eof_` first: a stream that finished and was *then* closed is
  still a clean `eof`.
- A zero-length read returns `{ok, 0}` and says nothing about end-of-body.
- **Check the state before parking.** `status()` used to `clear()` an event that `on_close()` had
  already set, and then waited forever (`d13e7b8`).

**Write side: the engine's ownership model decides your completion semantics.** This is the
biggest difference between the two engines:

| | nghttp2 (pull, copies) | nghttp3 (pull, *borrows until ACK*) |
|---|---|---|
| Engine behaviour | the data provider copies into the frame immediately | `read_data` hands out pointers that stay live until `acked_stream_data` |
| What we did | borrowed the caller's buffer descriptors, zero-copy (`f0f5859`) | **copy into a stream-owned staging area** (`kMaxStagedBytes = 256 KiB`) |
| `write_some` completes | after one provider pull (a true partial write) | as soon as the bytes are staged |
| Backpressure from | the peer's flow-control window | the staging bound |
| Cancel mid-write | leaves the stream usable | leaves the stream usable (borrowing would force a stream reset) |

Borrowing under nghttp3 would make each write wait a full round trip for its ACK, and
`capy::write`'s loop would pay that on every iteration. Traps found on the way:

- **nghttp2:** once a write is fully drained, reset "write pending" *synchronously* inside the
  provider callback. nghttp2 can call the provider again before the resumed coroutine runs, and a
  stale "pending" state produced hundreds of empty DATA frames (`9416f05`). With nothing to send,
  return `NGHTTP2_ERR_DEFERRED` and call `nghttp2_session_resume_data` on the next write.
- **nghttp3:** re-offering a range that has already been handed over *duplicates* those bytes on
  the wire. Track a cursor (`tx_next_chunk_`), and resume from it rather than rescanning from the
  front. Rescanning is quadratic with many small writes: it measured 70% of instructions and
  stalls of 170 ms (`519b470`).
- `capy::WriteStream` has no `write_eof()`. The public `Response`/`ClientRequest` carry it as a
  `std::function` closed over the `Stream`, the same way as `submit` and `get_response`.
- After `write_eof()`, a data write fails with `broken_pipe`, while an empty write and a repeated
  `write_eof()` stay no-ops. That matches anyhttp's README contract.
- A handler that returns without ending its body has abandoned the exchange. Reset the stream
  (`Stream::abort()`), or the peer waits forever (`db6e489`).
- A peer's STOP_SENDING or RESET_STREAM must wake your parked reader or writer yourself. Neither
  closes the stream while your own direction is still open (`d13e7b8`).

### 3.4 Flow control is split, deliberately (done here, both engines)

- **Connection-level credit is granted when the bytes arrive.** That window is shared with
  control, QPACK and other streams the application never reads, so withholding it stalls
  unrelated traffic.
- **Stream-level credit is granted when the application reads** (`Stream::consume()`). This is
  what makes a slow reader into backpressure, instead of an unbounded buffer inside the engine.
- nghttp2 needs `nghttp2_option_set_no_auto_window_update`, plus windows raised to 1 MiB. The RFC
  default of 64 KiB stalled every larger body on a WINDOW_UPDATE round trip (`dcda031`).
- nghttp3's `deferred_consume` covers bytes the application never sees, so it grants both halves.
- **ngtcp2 never gives stream slots back on its own.** Call
  `ngtcp2_conn_extend_max_streams_bidi/uni` on close, for peer-opened streams only. Without that
  the server goes silent after exactly `initial_max_streams_bidi` requests (`0099536`).

### 3.5 Public API shape (done here)

- PIMPL value types (`Server`, `Client`, `Session`) that are movable and non-copyable and hold a
  `shared_ptr<Impl>`. No engine or TLS type appears in a public header.
- `Session::Reader` = `capy::any_read_stream`, `Session::Writer` = `capy::any_write_stream`.
  Build on these stock erasers: the vendored stand-ins for the removed
  `any_read_source`/`any_write_sink` were retired once upstream stabilised (`f04cd15`).
- `Response::submit(status, headers)` is one explicit call, not a lazy accumulator.
- `ClientRequest::get_response()` is separate from the body writer, so the two can be combined
  with `when_all`. That is required when the server responds before it has read the whole
  request.
- `Server` owns its `io_context`. `Client` takes an external executor, so it can share a
  `Server`'s context, and then it shares that server's teardown as well (§4).

---------------------------------------------------------------------------------------------------

## 4. Lifetime and structured shutdown

### 4.1 The `TaskGroup` service (done here, `src/task_group.hpp`)

An `execution_context::service`: exactly one per `io_context`, found by type through
`ex.context().use_service<TaskGroup>()`. It holds a `std::stop_source` and a counter. Each
`spawn()` is a `run_async(ex, token, --count, --count)(task)`. Anything holding an executor for
that context can join it: the receive loop, every session, every request handler, and a `Client`
connected on the same executor. None of these components needs to know about the others.

### 4.2 Draining before destruction (done here)

```cpp
Server::Impl::~Impl() {
   auto& group = ioc_.use_service<detail::TaskGroup>();
   group.request_stop();
   while (group.count() > 0) {       // before ANY member destructs
      ioc_.restart();                // undo a latched stop()
      if (ioc_.poll() == 0) ioc_.run_one();
   }
}
```

Destroying an `io_context` while a coroutine frame is still suspended on it double-frees inside
`std::stop_state`/`stop_callback`. For weeks, the tests sidestepped this by leaking every `Server`
instead. With the drain, cancelled coroutines leave through their ordinary `if (ec) break;` paths,
and nothing is destroyed while it is suspended. Doing teardown properly for the first time
exposed two bugs:

- an accept loop that spun forever on a cancelled `accept()`, because it never checked
  `stop_requested()`;
- a `Stream`↔`Session::Impl` `shared_ptr` cycle for any stream the engine never closed. The fix
  is to close every remaining stream at the end of `run()`.

This is ungraceful by design, because a destructor cannot `co_await`. A graceful drain (GOAWAY and
letting in-flight requests finish) would be a separate `async_shutdown()`, and nothing like that
exists here yet.

### 4.3 Keep-alive idioms

`auto self = shared_from_this();` as the first line of every long-running member coroutine
(`run()`, `handle_request()`). Streams hold their session, and the session's map holds its
streams. §4.2 breaks the cycle.

### 4.4 `spawn` starts inline

`run_async` runs the coroutine up to its first suspension **before returning**. In
`handle_datagram()`, spawning a new session's `run()` before feeding it the Initial packet made its
first `flush()` run on a connection that had no Initial keys, and ngtcp2 asserted
(`e705948`). Order matters wherever "create, then spawn" is followed by more setup.

### 4.5 Crossing threads (done here, tests only)

`Server::stop()` is the only cross-thread entry point. The `External` fixture runs Boost.Process
on its own plain ASIO context and thread, and calls `server.stop()` from the `co_spawn`
**completion handler**, after the result promise has been fulfilled, rather than from inside the
coroutine (`c88c4f4`).

---------------------------------------------------------------------------------------------------

## 5. Going around corosio where it stops (done here, `src/quic_udp.hpp`)

corosio's datagram path never sets `msghdr::msg_control`, and `message_flags` is a closed enum.
That rules out GSO, GRO, packet info and ECN. The general pattern for any missing feature:

```cpp
co_await socket.wait(corosio::wait_type::read);   // readiness from corosio
recvmsg(socket.native_handle(), ...);             // transfer by hand; EAGAIN -> wait again
```

- The reactor keeps owning the epoll registration **and the cancellation path**, so structured
  shutdown (§4) still breaks these loops.
- It works because every socket corosio opens is `SOCK_NONBLOCK`.
- There is one wait-for-write slot per socket. Sessions that share a socket therefore serialize
  sends behind a `capy::async_mutex`, **held across the whole retry loop**, not one syscall.
- On `EAGAIN`, park and retry, and never drop the packet. A dropped packet looks like loss to
  ngtcp2 and collapses the congestion window over what is only local backpressure. (anyhttp drops
  here.)
- Make optional kernel features best-effort, with **sticky per-session fallbacks** (`no_gso_`,
  `no_ecn_`). Each degradation lands exactly on the previous behaviour.
- Value-initialise the send-side control buffer (`control[kControlSize]{}`). `CMSG_SPACE` pads
  beyond `CMSG_LEN`, `msg_controllen` covers that padding, and valgrind caught the kernel reading
  uninitialised stack on every GSO send.

Measured: +60% req/s on a 64 KiB echo from GSO/GRO, and 45 packets per `sendmsg()`.

Other places where the library had to step outside corosio:

- **TLS for QUIC.** corosio's `tls_context`/`openssl_stream` expose no `SSL*`, and they encrypt a
  byte stream into TLS records, which is not what QUIC needs. The library owns a raw `SSL_CTX`
  (`src/quic_tls.*`). `ngtcp2_crypto_conn_ref` attached with `SSL_set_app_data` is mandatory:
  without it, the handshake silently produces no CRYPTO frames and nothing reports an error.
- **Dual-stack.** corosio sets `IPV6_V6ONLY` on every `AF_INET6` socket except TCP acceptors.
  `native_socket_option::v6_only(false)` undoes it before `bind()`. A consequence for ECN: on an
  `AF_INET6` socket, Linux reports and honours an IPv4 peer's TOS only through `IP_TOS`, never
  `IPV6_TCLASS`.
- **TCP_NODELAY** has to be set explicitly on both ends. Without it, Nagle's algorithm combined
  with delayed ACKs held unpipelined HTTP/2 to 22 req/s. With it: 10.7k req/s (`f8a6ab8`).

---------------------------------------------------------------------------------------------------

## 6. capy/corosio trap list

Each of these cost real debugging time. The symptom column is what you will actually see.

| Trap | Symptom | Fix |
|---|---|---|
| `async_mutex::lock()` returns `io_result<>` and leaves the mutex **held** | the second caller blocks forever at 0% CPU | `auto [ec, guard] = co_await m.scoped_lock();` |
| An event is cleared after the operation that raced its `set()` | a silent hang under load | `clear()` at the top of the loop (§3.1) |
| A capturing coroutine lambda is invoked as a temporary, `[&]() -> task<> {...}()` | captures dangle after the first suspension | use free coroutine functions with parameters (`12d343f`) |
| `corosio::timeout()` on a composite operation | the value you get back is default-constructed, so progress is lost | report progress through an out-parameter (`count_into`, `send_into`) |
| `timeout()` skipped the inner awaitable's `await_ready()` | a segfault from `any_read_stream` (corosio#327, since fixed) | keep `test/issue_any_read_stream_timeout.cpp` |
| `openssl_stream` read and write in flight together | deadlock (corosio#330, fixed by #331) | keep `test/issue_tls_full_duplex.cpp` |
| `io_context::stop()` latches | `poll()`/`run()` return immediately | call `restart()` first |
| Unpinned `develop` dependencies | old commits fail to build | treat it as drift and look for an "adapt to upstream" commit |

The repository's habit: **every upstream bug becomes a small `test/issue_*.cpp` regression test**
under the `Issue` suite, with the issue link as a comment. When it gets fixed, the test tells you,
and if it regresses, the test tells you that too. `issue_udp_full_duplex.cpp` checks, before
anything is built on it, the guarantee that the whole QUIC design depends on: one `recv_from` and
one `send_to` in flight on the same socket.

---------------------------------------------------------------------------------------------------

## 7. Protocol-library traps, condensed

Each of these is explained in depth in `CLAUDE.md` or in the commit named.

**nghttp2** (`c88c4f4`): turn off automatic window updates; `NGHTTP2_ERR_DEFERRED` +
`resume_data`; the empty DATA flood (§3.3). Submitting response headers before any body existed cost
~40% throughput, because nghttp2 flushes a lone HEADERS frame and defers the provider (`bd287fd`).
That measurement is **HTTP/2 only** and has not been repeated under HTTP/3. A content-length
mismatch leads to a RST_STREAM, which surfaces as `stream_truncated`.

**ngtcp2:**
- `NGTCP2_WRITE_STREAM_FLAG_MORE` spins during the handshake if you retry blindly. Under
  `write_aggregate_pkt` it is safe, because `stream_id == -1` finishes the packet.
- Guard `nghttp3_conn_add_write_offset` on `h3 && stream_id >= 0 && ndatalen >= 0`.
- Build the HTTP/3 layer in `recv_rx_key(1RTT)`: the control and QPACK uni streams need 1-RTT keys.
- `NGTCP2_ERR_STREAM_NOT_FOUND` needs both shutdown and block, plus a guard against being handed
  the same stream a second time.
- Use `ngtcp2_conn_write_aggregate_pkt` rather than `_pkt2`, which anyhttp uses. It clamps the
  batch to the send quantum and handles pacing itself.
- **Not every `read_pkt` error may be answered.** `DROP_CONN`, `RETRY` and idle timeouts close
  silently. Record the error with `ngtcp2_ccerr_set_liberr`, and
  `ngtcp2_conn_write_connection_close` writes nothing for those types.
- Retry needs both halves: sending the Retry, and accepting its token back
  (`params.retry_scid` + `settings.token`). With only the first half, the failure just moves one
  round trip later. ~15% of curl connections failed under 20% receive loss without it.
- Register two connection IDs per new server session: its own SCID and the client's original DCID.

**nghttp3:** re-offering duplicates bytes; `acked_stream_data` is the only point where staged
bytes may be freed; `deferred_consume` grants credit at both levels.

---------------------------------------------------------------------------------------------------

## 8. Testing and verification recipes (done here)

- **Single-threaded end-to-end tests.** `Server` and `Client` share one `io_context`. The test
  coroutine is started with
  `run_async(server.get_executor(), [&]{ server.stop(); }, [&](auto){ server.stop(); })(...)`, then
  `run(ctx)`. No threads are involved, so the result is deterministic enough to run under valgrind.
- **`run_io_context()`** steps with `run_one()` at debug level and prints a separator per
  iteration, in red when one took ≥10 ms. That shows which reactor turn each callback ran on,
  which is how scheduler-level problems get tracked down.
- **Real peers.** The `External` fixture runs curl, h2load and osslclient against a live server,
  and `ExternalLossy` does the same with `--drop-rx`. Unit tests between two peers of the same
  library cannot catch interop bugs; that fixture found Retry, ECN peer differences and IPv4/IPv6
  issues.
- **Put tests beyond protocol limits, not on them.** Every acceptance bar of the HTTP/3 port was
  `-n 100`, exactly the stream limit, and so passed by a single request while the credit-return
  bug was present. `External.h2load` now uses `n = 1000`.
- **Loss knobs.** `Config::drop_rate_rx/tx` exercise retransmission, PTO and Retry without a lossy
  network. They cost nothing at their `0.0` default.
- **Loopback UDP drops.** A client and server on one thread overflow the receive buffer, and a lost
  retransmission tail waits for a ~70 ms PTO. Keep "stalled" thresholds well above 100 ms, and
  sequence tests with an `async_mutex` handshake instead of sleeps (`519b470`).
- **Hygiene gates** before calling anything done: the full suite under valgrind (`ValgrindMemcheck`,
  zero leaks), ASan/UBSan/TSan builds, and 20-50 shuffled repeats.
- **Benchmarks** use a Release build and **interleaved** A/B runs across two saved binaries, because
  this host varies by 40-90% between identical runs. `strace -e sendmsg,recvmsg` counts syscalls and
  control messages. That is what showed GSO working (374/394 sends) and h2load never doing ECN
  (2/413).
- **Spike before committing to an uncertain layer.** `ba418d5` proved corosio's `udp_socket` could
  carry QUIC, and found the four traps above, before any library code changed.

---------------------------------------------------------------------------------------------------

## 9. Recipes for porting anyhttp

anyhttp is this project multiplied by three protocols, TLS over TCP with ALPN, protocol detection,
h2c upgrade, Alt-Svc, a completion-token API, and multiple threads. Here is how each piece maps.

### 9.1 What can be lifted almost directly

| anyhttp | Starting point here | Porting effort |
|---|---|---|
| `h3_*` (ngtcp2/nghttp3) | `src/session.cpp`, `stream*.{hpp,cpp}`, `quic_*`, `server.cpp` | Low. Also bring back what this project does better: park on `EAGAIN` instead of dropping; `write_aggregate_pkt`; a source pin through `IP_PKTINFO` on send; ECN; Retry; the error handling that sends nothing for some closes; the read_data cursor. |
| `h2_*` (nghttp2) | `git show c88c4f4:src/{session,stream}.cpp`, `stream_impl.hpp` | Medium. The loops, the Stream bridge and flow control were done and tested. They need re-basing onto today's capy (tuple results) and today's Stream split. |
| `any_async_stream` | `capy::any_stream` | Delete it. capy's structural concepts already solve type-erasing a stream. |
| `impl::Reader`/`Writer` base classes | `capy::any_read_stream` / `any_write_stream` + `std::function`s for `submit`/`write_eof`/`get_response` | Delete the bases. |
| server-side session registry, `add_session` / `destroy()` | `detail::TaskGroup` | One mechanism for all three protocols. The drain loop replaces `destroyed_` and the sweep. |
| `logging.hpp`, `formatter.hpp`, `file_handler`, request handlers | already ported back here | Low. |
| anyhttp's test suite | `test/test_client_async.cpp` header | That header lists what does not port: immediate executors, cancellation slots, a URL type, `Request::reset()`. |

### 9.2 What is new and needs a decision

- **TLS library (decide first).** anyhttp links AWS-LC with `ngtcp2_crypto_boringssl`, and has a
  rule of one TLS library per process. corosio's TCP TLS is `openssl_stream` (or
  `wolfssl_stream`). This project moved to stock OpenSSL 3.5 with `ngtcp2_crypto_ossl` (its
  `SSL_set_quic_tls_cbs` API; quictls is not needed), so QUIC and TCP share one library.
  **Not tried:** whether corosio's `openssl_stream` builds against AWS-LC. Adopting OpenSSL 3.5 is
  the path with evidence behind it.
- **TLS and ALPN over TCP.** Done here, HTTP/2 (`8b2f511`): `openssl_stream(std::move(tcp), ctx)`,
  `co_await tls.handshake(tls_role::server)`, check `tls.alpn_protocol()`, then wrap the result in
  `capy::any_stream`, so the session never knows TLS was involved. Needs corosio#331 or later.
- **Protocol detection and h2c upgrade** (`detect_ssl`, `detect_h2`). **Not tried.** corosio has
  no detection helpers. The likely recipe is a small `PrefixedStream<S>` that satisfies
  `ReadStream`/`WriteStream`: `read_some` first returns the sniffed bytes, then delegates. Wrap it
  in `any_stream` and give it to either the TLS stream or the session. Write an `Issue`-style test
  for it first, because it sits beneath everything else.
- **HTTP/1.1 (Beast).** **Not tried.** Beast's `async_read`/`async_write` need an ASIO
  AsyncStream, so they cannot run over corosio. Beast's `http::parser` (`put()`) and
  `http::serializer` (`next()`/`consume()`) can be driven without I/O. That makes HTTP/1.1 a third
  instance of §3.1 (a receive loop feeding `put()`, a send loop draining the serializer), with
  `asio::const_buffer` converted to and from `capy` buffers at the boundary. Keep an eye on
  whether pulling in Beast headers drags in more ASIO than buffer types. anyhttp's "max concurrent
  streams = 1" and `would_block` rules live above this layer and carry over unchanged.
- **Completion tokens.** They do not carry over. The public API becomes coroutine-only
  (`io_task<...>`). If callback users matter, one `run_async` adapter at the edge is the whole
  bridge.
- **Threads.** Everything here depends on being single-threaded. `TaskGroup::count_` is not
  atomic, and `async_event` posts to its own executor. **Not tried:** the shape that keeps every
  recipe above valid is shared-nothing, meaning one `io_context` and one `Server` per thread over
  `SO_REUSEPORT` sockets. Note that for QUIC, `SO_REUSEPORT` distributes by 4-tuple, which breaks
  connection migration unless a BPF steering program routes by connection ID. Do threads last.
- **Alt-Svc and a shared port for TCP and UDP.** Structurally straightforward: both halves spawn
  into the same `io_context`'s `TaskGroup`, so one drain tears down both.

### 9.3 Suggested order

1. Shared infrastructure: `TaskGroup`, `run_io_context`, logging, formatters, the test fixture
   pattern from §8 with the `Issue` suite.
2. Decide the TLS library, then make QUIC work: lift `h3_*` from here.
3. TCP + TLS + ALPN with `any_stream`, then re-base `h2_*` from `c88c4f4`.
4. `PrefixedStream` and protocol detection, then h2c upgrade.
5. HTTP/1.1 through Beast's parser and serializer without I/O. Spike this first, the way
   `ba418d5` was, because it is the one layer with no evidence behind it yet.
6. Alt-Svc.
7. Threads, if ever.

At each step, port anyhttp's own tests for that protocol before calling it done. That is how
`db6e489` found four real bugs in code that was believed finished.
