# Plain QUIC streams, next to HTTP/3

The goal: QUIC streams as a byte-stream API of their own, with an ALPN chosen by the user and no
HTTP semantics, on the same ngtcp2 transport that carries HTTP/3 today. Planned 2026-10-16,
against ngtcp2 1.25.0, and built the same day on the `quic-streams` branch, steps 1 to 5. §7 has
where the result differs from the plan, and what is left.

---------------------------------------------------------------------------------------------------

## 1. Where HTTP/3 is coupled today

- **The session.** nghttp3 sits between ngtcp2 and the streams on every path. Each ngtcp2 stream
  callback forwards into `nghttp3_conn_*` (received data, acks, close, reset, STOP_SENDING, more
  credit), and `write_pkt()` lets `nghttp3_conn_writev_stream()` pick the next stream to write:
  nghttp3 is the scheduler. `setup_http3()` runs at the handshake, and the transport parameters are
  HTTP/3's (`initial_max_streams_uni = 3` is room for the control and QPACK streams).
- **The stream.** The read path is generic. The write path is shaped by nghttp3:
  - nghttp3 takes each piece of data once and keeps it, so a repeated request gets WOULDBLOCK
    (`write_offered`).
  - Acks arrive counted in body bytes, after nghttp3 has removed its framing.
  - `on_write_offered()` counts HEADERS framing too, hence its clamp.

  Beyond that, the headers, `submit_headers()`, the `!headers_submitted` branch of `delete_writer()`
  and the `NGHTTP3_H3_*` error codes are HTTP/3's.

---------------------------------------------------------------------------------------------------

## 2. Shape

| Layer | Directory | What it holds |
|---|---|---|
| `QuicSession`, `QuicServerSession`, `QuicClientSession` | `quic/` | ngtcp2 connection, TLS, timers, write loop, packet I/O; the UDP socket, CID demux and closing period (server) and connected socket (client) from today's `h3/server.cpp` and `h3/client.cpp` |
| `QuicStream` | `quic/` | read buffering, write state machine (ZeroCopy/Staged, cancellation, FIN intent vs. delivery), `fail()`, reader/writer lifecycle |
| `QuicApplication` | `quic/` | the seam where nghttp3 sits today, see below |
| `StreamApplication` | `quic/` | plain streams: callbacks straight to `QuicStream`, a round-robin write scheduler |
| `Http3Application`, `Http3Stream` (+ server/client streams) | `h3/` | `nghttp3_conn` and all `h3_cb_*`; headers, request dispatch |

**Composition for the protocol on top, inheritance only for the role.** The session owns a
`std::unique_ptr<QuicApplication>`:
- **Server:** created in the ALPN select callback. The application's transport parameters
  must reach `ngtcp2_conn_set_local_transport_params()` before
  `ngtcp2_conn_install_tx_handshake_key()`, and that callback is the last point before it.
- **Client:** created from `Config` before connecting. Its transport parameters go out in the
  ClientHello, before any ALPN is negotiated.

`on_ready()` runs where `setup_http3()` runs today (`cb_handshake_completed` / `cb_recv_rx_key`).
It can't be a `QuicSession → Http3Session` hierarchy:
- **The ALPN isn't known when the session is created.** The server creates the session on the
  first Initial packet, and the ALPN is only settled inside its first `ngtcp2_conn_read_pkt()`.
  Composition also lets one UDP socket serve `h3` and other ALPNs.
- **Role and protocol are independent.** Inheritance would mean four classes (server or client,
  times HTTP/3 or plain) or a diamond.

```c++
class QuicApplication
{
public:
   virtual int on_ready() = 0; // h3: control + QPACK streams
   virtual int recv_stream_data(int64_t id, std::span<const uint8_t> data, bool fin) = 0;
   virtual int acked_stream_data(int64_t id, uint64_t offset, uint64_t len) = 0;
   virtual int stream_close(int64_t id, uint32_t flags, uint64_t rx_code, uint64_t tx_code) = 0;
   virtual int stream_reset(int64_t id, uint64_t code) = 0;
   virtual int stop_sending(int64_t id, uint64_t code) = 0;
   virtual int extend_max_stream_data(int64_t id) = 0;

   // write scheduling, from write_pkt()
   virtual int next_stream_data(int64_t& id, bool& fin, std::span<ngtcp2_vec> vec) = 0;
   virtual int on_stream_data_written(int64_t id, size_t n) = 0;
   virtual void on_stream_blocked(int64_t id) = 0; // NGTCP2_ERR_STREAM_DATA_BLOCKED
   virtual void on_stream_shut_wr(int64_t id) = 0; // NGTCP2_ERR_STREAM_SHUT_WR / NOT_FOUND

   virtual void fill_transport_params(ngtcp2_transport_params& params) = 0;
};
```

Use `stream_close2` (new in 1.25.0): it reports the error codes of both directions.

**Directory boundary.** `quic/` includes `<ngtcp2/*>` but never `<nghttp3/*>`. CLAUDE.md's
`ninja -t deps` check gets a rule for that: nghttp3 only in `h3/`. That proves mechanically that the
transport has no HTTP in it.

---------------------------------------------------------------------------------------------------

## 3. The write path for plain streams

Facts from `ngtcp2.h` (1.25.0):
- ngtcp2 does not copy stream data. Memory handed to `ngtcp2_conn_writev_stream()` may be freed
  once `acked_stream_data_offset` reports it.
- `acked_stream_data_offset` comes in increasing offset order, without overlap. After
  `stream_close`, ngtcp2 touches none of the stream's data again.
- ngtcp2 keeps nothing it hasn't written yet. The unsent rest has to be passed again on every
  call.
- On a received STOP_SENDING, ngtcp2 sends RESET_STREAM by itself.
- A server may replace its transport parameters after choosing the ALPN, with
  `ngtcp2_conn_set_local_transport_params()`, until the handshake keys are installed.

What follows for `QuicStream`:
- **Offset, not "passed once".** The state becomes "sent up to offset". `Http3Stream` keeps the
  pass-once logic for nghttp3 on top of that.
- **Exact acks.** ZeroCopy writes complete exactly when their last byte is acknowledged; the
  HEADERS clamp has no plain-stream equivalent.
- **Staged chunks freed on ack**, instead of `in_flight_writes` living as long as the stream.
- **Flow control:** every byte is body. Stream credit only when the app reads (as now),
  connection credit when the data arrives.

The scheduler lives in `StreamApplication`. Streams with data to send wait in a round-robin list;
a blocked stream leaves the list until `extend_max_stream_data`. Check
`ngtcp2_conn_get_max_data_left()` first, as `write_pkt()` does now.

---------------------------------------------------------------------------------------------------

## 4. Public API

A plain stream maps almost 1:1 onto the Reader/Writer contract of the README ("The End of a
Body"):

| QUIC | Writer side | Reader side |
|---|---|---|
| FIN | `write_eof()` | `errors::eof` |
| RESET_STREAM | (`reset()`) | `errors::partial_message` |
| STOP_SENDING | the next write fails (§5, #4) | (`stop_sending()`) |

The after-the-end and cancellation rules carry over unchanged.

- **`quic::Stream`:** a Reader plus a Writer, with `id()`, `reset(code)` and `stop_sending(code)`.
- **`quic::Connection`:** `open_stream()`, `accept_stream()` and `close(code, reason)`.
  `open_stream()` fails with `errors::would_block` at the peer's stream limit, like HTTP/1.1's
  `submit()` (README, "Concurrent Requests").
- **Client:** `quic::connect(executor, host, port, {.alpn = ..., .tls_ca_file = ...})`.
- **Server:** an ALPN registered with a connection handler, `Task<void>(quic::Connection)`, on the
  existing `Server`, sharing its UDP socket and CID demux.

Both API styles, like the rest: coroutines in both, `async_*` with completion tokens in ASIO.

---------------------------------------------------------------------------------------------------

## 5. Open decisions

| # | Question | Decided |
|---|---|---|
| 1 | Unidirectional streams in the first version? | Bidi only: `initial_max_streams_uni` is 0. Uni fits later as a Writer-only / Reader-only stream. |
| 2 | Same port as HTTP/3, or a listener of its own? | Same port, dispatched by ALPN (`Server::on_quic()`). |
| 3 | Error code when a Writer is dropped without `write_eof()` | `quic::Config::reset_error_code`, default 0; STOP_SENDING for a dropped Reader has `stop_sending_error_code`. HTTP/3 keeps `H3_REQUEST_CANCELLED` / `H3_NO_ERROR`. |
| 4 | What a write reports after the peer's STOP_SENDING | `connection_reset`, as HTTP/3 does for a dead stream: `broken_pipe` already means "after your own end" in the Writer contract. |
| 5 | Out of scope for now | DATAGRAM (RFC 9221), 0-RTT, connection migration. |

---------------------------------------------------------------------------------------------------

## 6. Steps

Each step leaves both API styles building and the full suite green.

1. **Transport into `quic/`, nghttp3 behind `Http3Application`.** A pure refactor that changes no
   behavior, with an ASAN run (CLAUDE.md: HTTP/3 write-path lifetimes) and the new boundary
   check.
2. **Stream split:** `QuicStream` / `Http3Stream`. The write state moves from "passed once" to
   "sent up to offset" here; ASAN again.
3. **`StreamApplication`** with the scheduler and transport parameters of its own.
4. **Public API** (§4) and the server's ALPN registry.
5. **Tests**, parametrized where they can be: echo, FIN, reset → `partial_message`, a write after
   STOP_SENDING, a slow reader keeping the peer's window closed, `open_stream()` at the limit →
   `would_block`, cancellation in both write modes, a ZeroCopy write completing on its ack.
   No outside plain-QUIC peer is installed. ngtcp2's `h09` examples (hq-interop) could be built
   next to `osslclient`/`osslserver` for one.

---------------------------------------------------------------------------------------------------

## 7. As built

The shape is §2's. Where the details came out differently:

- **Two STOP_SENDING callbacks.** ngtcp2's `stream_stop_sending` is about *our* read side (we
  called `ngtcp2_conn_shutdown_stream_read()`); the peer's STOP_SENDING arrives through
  `recv_stop_sending`, which was not installed before. `QuicApplication` has both, named after
  them. Also new: `stream_open` (a stream the peer opened -- implicitly opened ones get no call,
  so the first callback for an unknown remote ID creates the stream too) and `on_closed()`.
- **`stream_close`, not `stream_close2`.** One code, "set or not", is all the mapping needs: a
  stream that was reset either way fails what is pending with `connection_reset`.
- **The bare FIN is reported.** `write_pkt()` hands `on_stream_data_written()` zero-byte writes and
  whether the FIN went with them, as ngtcp2's examples do (`ndatalen >= 0`). HTTP/3 ignores both.
- **Half-close in `QuicStream`:** `abort_reading()` / `abort_writing()` and the `read_aborted` /
  `write_aborted` flags, next to `closed` (both directions over). HTTP/3 doesn't use them and
  keeps aborting the whole exchange: `reset_writing()` and `delete_writer()` are virtual, and
  `Http3Stream` overrides them with what it did before.
- **Adapters.** `QuicReader<Interface, Stream>` / `QuicWriter<...>` are the generic halves of what
  were `Http3Reader` / `Http3Writer`; those now derive from them and add the header section. A
  plain stream's handle is one of each.
- **The server's connection handler is posted**, not started inside `on_ready()`: that runs inside
  ngtcp2, before the session counts as ready.
- **Teardown re-entrancy.** Destroying a stream completes its handlers inline, which may drop the
  `Connection` handle, which closes the connection. `clear_streams()` therefore moves the map out
  before destroying it, and `close_connection()` always posts (ASIO showed it as a double free).
- **API spelling.** `Stream::reset_stream(code)` rather than `reset(code)`: `reset()` already
  releases a handle, on both of its bases. `open_stream()` is synchronous and returns the tuple
  directly; `accept_stream()` and `quic::connect()` are operations, with completion tokens in ASIO.
  The server's side is `Server::on_quic(config, handler)`, with the ALPN in the config.
- **Log prefixes.** A server session is `quic:` until the ALPN is chosen, then the ALPN; the
  handshake line says "<alpn> over QUIC"; "session destroyed" counts streams, not requests.

Tests: `test/test_quic.cpp`, every case in both write modes, plus the completion token spelling in
ASIO -- echo, large bodies over several streams, half-close, reset → `partial_message`, a write
after STOP_SENDING, a dropped Writer, a slow reader holding the writer back, `open_stream()` at the
limit, server-initiated streams, connection close, cancellation, an unknown ALPN, HTTP/3 next to
plain streams on one port.

Left open:
- Unidirectional streams, DATAGRAM, 0-RTT, migration (§5).
- An outside peer: ngtcp2's `h09` examples (hq-interop) could be built for interop tests.
- The `client` and `server` programs have no plain QUIC mode.
- A zero-copy write cancelled while nothing is unacknowledged leaves the stream alive -- a
  shorter body -- which is right, but makes "cancel resets the stream" hard to test on loopback.
