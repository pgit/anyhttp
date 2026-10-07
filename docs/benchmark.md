# Benchmarks

What has been measured about the standalone server (`src/apps/server_main.cpp`), how, and what came
out of it. Newest findings come first. Each section says how to reproduce it; the commit named
there has the full story.

All numbers come from one machine, an i7-12700KF (8 P-cores with SMT and 4 E-cores, 20 hardware
threads) under WSL2, with h2load from nghttp2 1.70 over loopback. Treat them as relative. On this
host identical runs can differ by 40-90%, so compare A and B within one session, interleaved,
never against a number in this file.

## Contents

- [How to measure](#how-to-measure)
- [Threads: one shared context or one context per thread](#threads-one-shared-context-or-one-context-per-thread)
- [ASIO vs COROSIO](#asio-vs-corosio)
- [Earlier findings](#earlier-findings)
- [Bugs found while benchmarking](#bugs-found-while-benchmarking)

---------------------------------------------------------------------------------------------------

## How to measure

### `scripts/bench.sh`

```
scripts/bench.sh [-D seconds] [-c clients] [-m streams] [-t threads] [-i] [-p port] [-u path] [-P] [-n] [-N] [-v]
```

It builds and starts the server of `build-openssl-release/` (ASIO) and `build-corosio-release/`
(COROSIO) one after the other, runs h2load over HTTP/1.1, HTTP/2 and HTTP/3 against each, and
prints a table of req/s, MB/s, failures and median/p99 latency, plus COROSIO/ASIO per protocol.
Both trees use OpenSSL, because COROSIO has to; `build-release/` is ASIO on AWS-LC, which is
faster on its own (see [TLS library](#tls-library-aws-lc-vs-openssl)) and would skew the
comparison.

nghttpd, if it is on the PATH, adds an HTTP/2 row and its ratio to ASIO as a reference: the same
nghttp2 and OpenSSL without anyhttp around them. It serves files from a document root that matches
the server program on `/` ("Hello, World!") and `/test/*`; other paths are 404s there and show up
as failed. With `-t N` it runs N workers, each an event loop to which the accepting thread hands
connections round robin, which is closer to `-i` than to a shared context.

| option | meaning |
|---|---|
| `-t N` | the servers run on N threads, h2load gets as many (up to the clients) |
| `-i` | with `-t`: one I/O context and one server per thread instead of one shared context, see below |
| `-M N` | h1 pipelining depth, default 1 (no pipelining), see below |
| `-P` | plaintext HTTP/1.1 and HTTP/2 (prior knowledge), no HTTP/3 |
| `-u path` | request path, default `/` ("Hello, World!", nothing else) |
| `-n` | don't build first |
| `-N` | leave out nghttpd |
| `-v` | print the command line that starts each server |

h1 runs with `-m 1` unless `-M` says otherwise: with `--h1`, h2load's `-m N` pipelines N requests
per connection, which real clients hardly do. It measures fewer reads and writes per request
rather than anything the server does concurrently, so h1 numbers taken with `-M` are not
comparable to the others. It is a stress test: a full buffer of requests, abandoned when h2load
stops.
Raw h2load output and server logs stay in a temporary directory that the script prints at the end.

The tables below use `-c 32` unless they say otherwise.

### Hygiene

- **Release trees only.** Never benchmark `build/` (Debug).
- **Check that the server is the one listening.** A failed `bind()` aborts quietly, and h2load then
  measures whatever else owns the port. `bench.sh` refuses a port that is in use and waits for
  `UDP listening` (once per thread with `-i`). With `-i` the servers bind with `SO_REUSEPORT`, so a
  leftover server that also did would silently get part of the load.
- **Ports from 18080 up**, away from servers you run yourself on 8080/8081.
- **Explicit PIDs.** Keep `$!` and kill that. `pkill -f src/server` also matches the shell running
  the command.
- **Uploads go to `/upload`**, which drains the body before it responds. h2load stops sending a body
  once the response is complete, so `/eat_request` (responds first) moves about 10% of the data.
  Check the bytes actually moved with the `lo:` counters in `/proc/net/dev`.
- **Interleave A/B runs** across two saved binaries, several rounds each, and report the range.

### Tools that work here

`perf_event_paranoid` is 2 and ptrace attach is not permitted, so:

- `perf record -g -p PID` works, but sees **user space only**.
- `gdb -p` and `strace -p` fail. `strace -c -f` and valgrind/callgrind work when they *launch* the
  server.
- `/proc/PID/stat` fields 14 and 15 (utime, stime) before and after a run give user and system CPU
  per request without disturbing anything:

  ```bash
  a=($(awk '{print $14, $15}' /proc/$pid/stat))
  h2load -D 6 -c 32 -m 10 -t 4 https://127.0.0.1:18080/ > h2load.log
  b=($(awk '{print $14, $15}' /proc/$pid/stat))
  reqs=$(awk '/^requests:/{print $8}' h2load.log)
  echo "user $(( (b[0]-a[0]) * 10000 / reqs )) us/req, sys $(( (b[1]-a[1]) * 10000 / reqs )) us/req"
  ```

- `/proc/PID/io` `syscr`/`syscw` do **not** count `sendmsg`/`recvmsg`, so they say nothing about
  socket I/O.
- For a gap that depends on scheduling, temporary atomic counters (wake-ups, flushes, packets,
  datagrams, timer arms) dumped at shutdown and compared per request with `-c 1 -m 1` found more
  than any profile did (see [ASIO vs COROSIO](#asio-vs-corosio)).

---------------------------------------------------------------------------------------------------

## Threads: one shared context or one context per thread

*2026-10-03, `179f4be`.* capy's documentation names two patterns for using several threads,
["multi-threaded with shared data" and "multi-threaded with independent work"][capy-patterns]. In
anyhttp terms:

- **Shared** (`server -t N`): one I/O context run by N threads, with a strand per connection
  (`Config::use_strand`). Executor affinity keeps a connection and everything it awaits on its
  strand. A connection is several coroutines (send and receive loops, one handler per request),
  so on a context with several threads the strand cannot be dropped.
- **Independent** (`server -t N --independent`): N contexts with one thread and one `Server` each,
  no strands. The servers share the port through `SO_REUSEPORT` (`Config::reuse_port`), and the
  kernel spreads TCP connections and UDP datagrams over them, by address and port. Nothing is
  shared but the signal handler, which stops each server on its own executor.

[capy-patterns]: https://develop.capy.cpp.al/capy/4.coroutines/4d.executors.html#_single_threaded_vs_multi_threaded_patterns

### Results

`bench.sh -c 32 -t N` with and without `-i`, TLS, req/s, ASIO / COROSIO. Bold is the best of the
four per thread count and protocol:

| threads | model | h1 | h2 | h3 |
|---|---|---|---|---|
| 1 | | 73k / 86k | 212k / 251k | 120k / 129k |
| 2 | shared | 70k / 101k | 168k / **414k** | 108k / 184k |
| 2 | independent | 128k / **150k** | **402k** / 368k | **240k** / 223k |
| 4 | shared | 99k / 149k | 255k / **768k** | 176k / 341k |
| 4 | independent | 237k / **280k** | **739k** / 633k | **488k** / 418k |
| 8 | shared | 105k / 199k | 395k / **1126k** | 295k / 548k |
| 8 | independent | 247k / **270k** | **1074k** / 838k | **660k** / 592k |

At 8 threads the server and h2load together want 16 of the 20 hardware threads, partly on SMT
siblings and E-cores, so those numbers are bound by the client as much as by the server. h1
levels off there in both models.

CPU per request at 4 threads, user + system, in µs (`/proc/PID/stat`, `h2load -D 6 -c 32 -t 4`,
`-m 10`, h1 `-m 1`):

| | h1 | h2 | h3 |
|---|---|---|---|
| ASIO shared | 23.1 + 13.5 | 10.7 + 2.8 | 14.1 + 5.0 |
| ASIO independent | 9.3 + 6.9 | 4.6 + 0.8 | 6.9 + 1.7 |
| COROSIO shared | 14.5 + 11.5 | 4.2 + 0.9 | 7.7 + 2.9 |
| COROSIO independent | 8.7 + 6.8 | 4.9 + 1.3 | 6.9 + 2.3 |

Scaling across independent contexts, h2 (`h2load -D 6 -c 32 -m 10 -t 4`, req/s):

| contexts | ASIO | COROSIO |
|---|---|---|
| 1 | 187k | 215k |
| 2 | | 337k |
| 4 | 733k (3.9x) | 614k (2.9x) |
| 8 | | 979k |

### What it means

- **ASIO's poor thread scaling came from the threading model, not from ASIO.** One context shared
  by all threads barely scales: h2 goes from 212k to 255k at 4 threads. With a context per thread
  it scales linearly and needs less than half the CPU per request on every protocol.
- **COROSIO's threaded scheduler is far better than ASIO's shared one,** but a context per thread
  takes most of its lead away. In the independent model COROSIO stays ahead on h1 only and is
  7-22% behind on h2 and h3.
- **h1 and h3 gain from independent contexts in both styles.** COROSIO h1 drops from 26 to 15.5 µs
  per request, 4.7 µs of the saving in system time.
- **COROSIO h2 gets slower** with independent contexts: 6.2 instead of 5.0 µs per request, with
  system time up by half. Load is spread evenly (all threads equally busy), and
  `--inline-budget 0` changes nothing, although inline completion is back on with one thread per
  context. A guess, not verified: a strand runs a connection's queued handlers together, so its
  writes coalesce, while a context's single FIFO interleaves
  connections and flushes each one more often. User-space profiles of the two look alike; a
  per-request count of TLS writes and `sendmsg()` calls would settle it.
- **COROSIO scales below linear across independent contexts** (2.9x at 4), ASIO doesn't. Shared
  state between corosio/capy contexts is a suspect: the independent profile shows
  `recycling_memory_resource::deallocate_slow` and lock waits that the shared one hardly does,
  but at about 2% together they don't explain it.
- **The independent model doesn't trigger the corosio reactor's use-after-free.** That bug needs
  more than one thread in the same context's `run()`, see
  [below](#corosio-epoll-reactor-use-after-free-with-several-threads).

### Limitations of the independent model

- A QUIC connection whose client address changes (migration, NAT rebinding) lands on a server that
  does not know its connection ID. Doing it properly needs eBPF steering by connection ID, or
  forwarding through a shared table. h2load never migrates, so the benchmark is not affected.
- All servers have to be bound before traffic arrives: each one that joins reshuffles where
  datagrams go.
- Connections stay on the context that accepted them, so a few long-lived connections can leave
  threads idle. `-c 32` hides that; `-c 4 -t 4` would show it.

### Reproduce

```bash
for t in 1 2 4 8; do scripts/bench.sh -c 32 -t $t; scripts/bench.sh -n -c 32 -t $t -i; done
```

---------------------------------------------------------------------------------------------------

## ASIO vs COROSIO

*2026-10-02.* When `bench.sh` was first written, COROSIO was behind on everything over TLS. Two
causes, both fixed:

1. **h1, 24k vs 52k req/s.** corosio's `openssl_stream` encrypts only the first buffer of a
   sequence into a TLS record and sends it at once. Beast's serializer hands over a response head
   as a dozen small buffers, so each response went out as nine records and nine `send()`s (ASIO:
   two). `TlsStream` now copies small buffers into one first, as `asio::ssl::stream` does: 24k →
   66k req/s (`963f45c`).
2. **h3 at 0.77x, h2 at 0.95x.** `initiate()` posted the caller's resumption even when the operation
   completed inside the initiating call, which `Response::submit()` always does. HTTP/3's write
   pass, posted by `wake_write()`, then ran first and sent each response head in a packet of its
   own, before the handler could add the body: twice the flushes, packets and ACKs of ASIO. Found
   by counting events per request in both styles, not by profiling. Fixed by resuming at once,
   after the initiating call has returned (`6c3c50f`): h3 97k → 143k (ASIO 124k), h2 197k → 247k
   (ASIO 207k), h2 cleartext 229k → 276k (ASIO 191k), with `bench.sh -D 3`.

Ruled out: corosio writes its eventfd on every timer arm, even when no thread is waiting in
`epoll_wait()`. Patching that away removed the syscalls and changed nothing measurable. Also seen,
not acted on: `udp_socket::wait()` makes about three `poll()` calls per wake-up, and COROSIO's
`Timer::arm()` launches a coroutine each time.

**Inline budget.** corosio completes I/O that is ready at once inline, up to an adaptive budget,
instead of posting it. With more than one thread in a context and all budgets at their defaults it
posts everything. `server --inline-budget N` / `--unassisted-budget N` (COROSIO only) set them.
`--inline-budget 0` (post everything) left COROSIO h2 with independent contexts unchanged
(608k vs 623k req/s at 4 threads); the other combinations have not been measured.

### Reproduce

`scripts/bench.sh` (single-threaded), and `scripts/bench.sh -P` for cleartext.

---------------------------------------------------------------------------------------------------

## Earlier findings

ASIO, single-threaded, before `bench.sh` existed: plain h2load runs against
`build-release/src/server`, most of them interleaved A/B against the parent commit. The commit
named has the method.

### HTTP/2

- **Posting the send loop's wake-up** lets several `start_write()`s share one pass: small GETs
  170-173k → 203-205k req/s (h2c, `-c 10 -m 10`), a 64 KiB echo unchanged at 22-26k (`8460a37`).

### HTTP/1.1

- **Beast's parser and serializer driven by our own coroutines** instead of Beast's asynchronous
  operations (`12bf9e3`): small GETs 52.5-54.0k → 54.1-55.4k req/s, a 64 KiB echo 24-29k →
  30-40k, 3.1k fewer instructions per request (callgrind). The coroutine shape costs HTTP/2 one
  frame: 201-205k → 197-199k.

### HTTP/3

- **GSO and GRO** (`df471aa`), 1000 × 1 MB, `-c 10`, MB/s:

  | | download | upload |
  |---|---|---|
  | both on | 1385 | 1277 |
  | GRO off | 1409 | 752 |
  | GSO off | 565 | 1256 |
  | both off | 552 | 741 |

  Reproduce with `server --disable-gso` / `--disable-gro`.
- **Fill a read from every queued chunk**, not just the first (`c4b4595`): a 64 KiB echo went from
  48 reads of ~1.4k per request to 3, and from ~300-450 to ~7300 req/s at 7x less CPU per request.
  A body write completes only on acknowledgement, so a handler that answers each read with a
  write paid a round trip per packet.
  `h2load --h3 https://localhost:8080/echo -d test/data/64kminus1 -n 1000 -c 4 -m 3`
- **Write once per receive batch**, not per datagram (`62021e9`): 9.85 → 7.0 syscalls per request,
  no change in throughput (bound by user-space CPU, ~57 µs per request then).
- **Arm the write flush once per wake** (`b0a4b68`): `timerfd_settime` 1.74 → 0.54 per request, no
  change in throughput.
- **AES-128-GCM first** among the QUIC cipher suites (`bf34a2c`), as ngtcp2's example server does.
  Worth a few percent of bulk throughput with AES-NI.

### TLS library: AWS-LC vs OpenSSL

AWS-LC (the default, `build-release/`) needs about 55% less CPU per TLS handshake over TCP and 35%
less over QUIC, and echoes 8-10% more MB/s on all three protocols (`b4b15e5`). With `bench.sh`
on 2026-10-02, ASIO h1 over TLS fell from about 80k to 52k req/s on OpenSSL. That is why
`bench.sh` compares `build-openssl-release/` with COROSIO, not `build-release/`.

### Smaller

- A real `Date` header slows the server down measurably (`7528604`).

[`capy-corosio-lessons.md`](capy-corosio-lessons.md) has further measurements (GSO packets per
`sendmsg()`, `TCP_NODELAY`, response headers before the body) taken in its sibling project,
nghttp3-corosio, not in anyhttp.

---------------------------------------------------------------------------------------------------

## Bugs found while benchmarking

### corosio: epoll reactor use-after-free with several threads

With more than one thread in a corosio context's `run()`, `epoll_scheduler::run_task()` takes each
event's `reactor_descriptor_state*` from `epoll_wait()` and calls `add_ready_events()` on it
without its lock. That state lives in the socket's implementation. A socket destroyed on another
thread at that moment is freed: `do_close_socket()` pins the implementation only once
`is_enqueued_` is set, which the reactor thread has not done yet.

[`src/issues/corosio_issue_reactor_uaf.cpp`](../src/issues/corosio_issue_reactor_uaf.cpp)
reproduces it with capy and corosio only: coroutines on a 4-thread context create a socket pair,
write a byte and destroy both sockets.

```bash
cmake --build build-corosio-asan --target corosio_issue_reactor_uaf
build-corosio-asan/src/corosio_issue_reactor_uaf 4     # heap-use-after-free within a second
build-corosio-release/src/corosio_issue_reactor_uaf 4  # most runs: "stuck after N of M rounds"
build-corosio-release/src/corosio_issue_reactor_uaf 1  # always completes
```

Without a sanitizer most runs stall part way, presumably because the stale event lands on the
next socket allocated at the same address and its write never completes. For a server that means
connections that hang, not a crash. Not filed upstream yet. The independent model (one thread per
context) avoids it.

### ASIO HTTP/1.1: shutdown hangs after pipelined requests

`h2load --h1 -m 10` pipelines ten requests per connection. When h2load stops at the end of `-D`
with requests still in flight, the ASIO server's HTTP/1.1 sessions never finish: on SIGINT it
logged `waiting for 32 sessions` and hung, although no TCP connection was left. COROSIO shut down
cleanly.

```bash
build-openssl-release/src/server -p 18080 & pid=$!
h2load --h1 -D 2 -c 32 -m 10 -t 4 https://127.0.0.1:18080/
kill -INT $pid   # never exited
```

The sessions were stuck before the signal. After the first failed response, a session went on to
the requests still in its buffer, which it can parse without reading from the connection. ASIO's
epoll reactor treats a failed send like a short write: it stops trying writes right away and waits
for the socket to become writable, which a reset socket signals only once under edge triggering.
So the third write on the dead connection never completed. `destroy()` only shut the socket down,
which on a reset connection fails with `ENOTCONN`. Fixed: a failed response ends the session, and
`destroy()` also cancels what is pending
(`ConnectionClose.WHEN_peer_resets_with_requests_pipelined_THEN_the_rest_are_dropped`).

### `server::Server` destroyed after a move

Destroying a moved-from `Server` dereferenced null, which a `std::vector<std::optional<Server>>`
does as it grows. Fixed in `3b12023`.
