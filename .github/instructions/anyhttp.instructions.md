---
applyTo: '**'
---
AnyHTTP is an experimental, low-level C++23 HTTP client and server library with a type-erasing
interface: one API for HTTP/1.1, HTTP/2 and HTTP/3 (QUIC), hence the *any* in the name -- plus
plain QUIC streams next to HTTP/3. None of the protocols are implemented from scratch; it wraps
Boost.Beast (parser and serializer), nghttp2, and ngtcp2/nghttp3.

Everything is asynchronous and written with C++20 coroutines. Every operation is a coroutine
(`read_some`, `write`, `write_eof`, `submit`, `get_response`, `connect`, `get`) that yields a
tuple and reports errors instead of throwing.

There are two API styles, chosen at configure time with `-DANYHTTP_API=`:

* **ASIO** (default), on Boost.Asio: every operation is also an ASIO asynchronous operation
  (`async_x(..., token)`) and takes any completion token, adhering to its [async model].
* **COROSIO**, on [capy](https://github.com/cppalliance/capy) and
  [corosio](https://github.com/cppalliance/corosio): coroutines only.

Both styles build the same library sources and tests; code that belongs to one of them is guarded
with `ANYHTTP_ASIO` / `ANYHTTP_COROSIO` (exactly one is 1, from the generated `anyhttp/config.hpp`)
and lives in `asio/` or `corosio/`, under both `include/anyhttp/` and `src/`. Generic code reaches
the runtime only through `anyhttp/runtime.hpp`, `anyhttp/net.hpp` and `anyhttp/formatter.hpp`, and
includes nothing else from Boost.Asio but `<boost/asio/buffer.hpp>`.

Each protocol has a directory of its own, again under both `include/anyhttp/` and `src/`: `h1/`
(Beast), `h2/` (nghttp2), `quic/` (ngtcp2) and `h3/` (nghttp3, on top of `quic/`). Only files in
`h2/` include `<nghttp2/*>`, only `quic/` and `h3/` `<ngtcp2/*>`, only `h3/` `<nghttp3/*>`.
Generic code reaches a protocol through that protocol's `backend.hpp`. Keep these boundaries.

Conventions: clang-format is authoritative (3-space indent, 100 columns); types are `CamelCase`,
functions `snake_case`, members `foo_`. Commit subjects are prefixed `fix:`, `refactor:`, `docs:`,
`chore:`, `style:`, `test:`, or with the protocol (`h1:`, `h2:`, `h3:`).

Builds are out-of-source, one tree per configuration (`build/` Debug, `build-release/` Release,
`build-corosio/` the COROSIO style, plus sanitizer trees); `scripts/configure.sh` sets them up.
Run the tests from the repository root -- they load TLS material through relative paths -- and
prefer `gtest-parallel build/test/test_all`.

[README.md](../../README.md) documents the API, the concurrency model and the reasoning behind it:
read it before changing anything about request/response semantics.
[CLAUDE.md](../../CLAUDE.md) covers working in the tree in detail -- the build trees and their
options, how to run and filter the tests, the sanitizer recipes, the source layout and the traps.

[async model]: https://think-async.com/Asio/boost_asio_1_30_2/doc/html/boost_asio/overview/model.html
