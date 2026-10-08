# Working on anyhttp

C++23 HTTP/1.1 + HTTP/2 + HTTP/3 server and client on top of Boost.Asio. [README.md](README.md)
describes the API, the concurrency model and the reasoning behind it -- read it before changing
anything about request/response semantics. This file is about *how to work in the tree*.

## Build and test

Out-of-source trees, one per configuration. `build/` is Debug, `build-release/` is Release;
benchmark with the latter, never with `build/`. Both use clang, which builds against libc++ and
the libraries in `/opt/libc++` (`USE_LIBCXX`, `LIBCXX_ROOT`); `build-gcc/` is GCC with libstdc++
and `/usr/local`. Keep `build/` on clang: clangd reads its `compile_commands.json`.

`build-corosio/` is the COROSIO API style (`-DANYHTTP_API=COROSIO -DTLS_LIBRARY=OpenSSL`, see
[docs/corosio-port-plan.md](docs/corosio-port-plan.md)); capy and corosio are FetchContent'd at
pinned SHAs. Both styles build the same library sources and test files; what belongs to one style
only is guarded with `ANYHTTP_ASIO` or `ANYHTTP_COROSIO` (exactly one of them is 1). So is the
`server` program; the `client` program is ASIO-only so far. `build-corosio-asan/` is its ASAN
tree, `build-corosio-release/` its Release tree (benchmark COROSIO with it). COROSIO needs
OpenSSL, so compare it against `build-openssl-release/` (ASIO, OpenSSL, Release), not
`build-release/`: `scripts/bench.sh` runs the two side by side (`-P` for plaintext, `-t` for
server threads).

`build-corosio-tsan/` is COROSIO under TSAN with `-DMULTITHREADED` in `CMAKE_CXX_FLAGS`: the
fixtures run the tests on all cores, a strand per connection. Expect reports inside corosio's
epoll reactor, which frees a socket's descriptor state while another thread still handles an
event for it (see the port plan, step 8); a report with an anyhttp frame at the racing access is
ours. A few tests set up their own server or client on the bare context and are not
thread-aware.

`scripts/configure.sh` configures all of these trees, or the ones named, with their options (`-b`
builds them too); `scripts/bench.sh` configures its two when they are missing. Name the compiler
when configuring a tree by hand: `cc` and `c++` are GCC on this host.

```
scripts/configure.sh build
cmake --build build
build/test/test_all
```

**Run the tests from the repo root**, never from inside a build tree: the servers load their TLS
material and the clients their CA through relative paths (the defaults of
`server::Config::tls_certificate_chain` / `tls_private_key`, and `pki/out/root.pem`, which the
test fixtures set as `client::Config::tls_ca_file`). From the wrong working directory every test
fails in `SetUp()` with "use_certificate_chain_file: No such file or directory".

The full suite takes about 50s serially (h2spec and the HTTP/3 timing tests dominate). Anything
running much longer means a test is hanging; kill it and run that test alone.

For routine runs use `gtest-parallel build/test/test_all` (same working-directory rule): about 7s.
Every server binds an ephemeral port, so parallel processes do not collide. It prints only
failing tests and does not report skips, and since each test gets its own process it cannot
catch interference through process-wide state (statics, the logger) -- do a serial run before committing changes to shared or global state.

Parametrized tests are suffixed `/HTTP11`, `/HTTP2`, `/HTTP3` -- not h2/h3. `--gtest_filter` knows
only `*` and `?`, and a filter that matches nothing exits 0 with no output, which reads like a
pass: check for `[ OK ]` lines.

### Sanitizers

Separate trees; the sanitizer options in the Debug branch of `CMakeLists.txt` are commented out on
purpose, so configure a new tree instead of editing them:

```
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
build-asan/test/test_all
```

GTest is built from source with the tree's own flags (`test/CMakeLists.txt`), so it is
instrumented too and needs no `ASAN_OPTIONS`. The `Recursion` test skips itself under ASAN, so
expect 3 extra skips.

TSAN is the same recipe with `-fsanitize=thread` and runs clean without suppressions. AWS-LC is
uninstrumented, but linked statically into the executable, so `called_from_lib:` cannot match it:
should a report ever show *both* stacks inside the TLS library, suppress it with `race:` on the
AWS-LC function instead. One with an anyhttp frame near the racing access is real.

Anything touching lifetimes on the HTTP/3 write path belongs under ASAN before it is committed --
completions there run on acknowledgement, and the Release build hides the use-after-frees.

## Source layout

Each protocol lives in a directory of its own, `h1/` (Beast/HTTP/1.1), `h2/` (nghttp2) and `h3/`
(ngtcp2+nghttp3), in both `include/anyhttp/` and `src/`. **Only files in `h2/` may include
`<nghttp2/*>`, only files in `h3/` `<ngtcp2/*>`/`<nghttp3/*>`.** What belongs to one API style only
is in `asio/` or `corosio/`, again in both; the library compiles the `src/` directory of its style
and not the other. `src/apps/` has the `server` and `client` programs, `src/issues/` standalone
reproducers of upstream bugs. Everything else at the top level is generic.

Generic code (`server_impl.*`, `client_impl.*`, `formatter.hpp`) reaches a backend only through
`h1/backend.hpp` / `h2/backend.hpp` / `h3/backend.hpp`, which declare their entry points in terms
of the runtime layer. New protocol-specific code goes in that protocol's directory; when generic
code needs to reach it, add a declaration to that backend header rather than including a protocol
header.

Verify the boundary with:

```
ninja -C build -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp2\/nghttp2.h|ngtcp2\/ngtcp2.h/{print o}' | sort -u
```

Only objects in `h2/` and `h3/` of the library may appear. (Test objects may: the h2c upgrade and
formatter tests drive nghttp2 by hand.)

**Generic and protocol code include nothing from Boost.Asio but `<boost/asio/buffer.hpp>`**, which
Beast's parser, serializer and `Fields` bring anyway, and nothing from capy or corosio. They reach
the runtime through `anyhttp/runtime.hpp`, `anyhttp/net.hpp` and `anyhttp/formatter.hpp`, which
include the `asio/` or `corosio/` header of the build's style. Addresses and endpoints are the
runtime's own (`IpAddress`, `TcpEndpoint`, `UdpEndpoint`); where the two differ, `net.hpp` declares
a function each runtime defines (`io::make_address()`, `io::to_sockaddr()`, ...). Verify in a
COROSIO tree that no library object includes more of Asio:

```
ninja -C build-corosio -t deps | awk '/^[^ ].*: #deps/{o=$1} /boost\/asio\// && !/boost\/asio\/((buffer|is_contiguous_iterator|version)\.hpp|detail\/)/{print o}' | grep anyhttp.dir | sort -u
```

It must print nothing. (Test objects may: `test_external.cpp` runs its child processes on an ASIO
context in both styles.)

The HTTP/3 server and client share one implementation: `h3/session.*` (all ngtcp2/nghttp3
callbacks, packet writing, timers, flow control), `h3/stream.*` (read and write paths, header
parsing, lifecycle, the reader/writer adapters), `h3/common.*` (helpers). `h3/server.cpp` and
`h3/client.cpp` hold only what is genuinely role-specific. Fix shared behavior in the shared
files.

## Conventions

- clang-format is authoritative: 3-space indent, 100 columns.
- The two API styles are ASIO and COROSIO: `ANYHTTP_ASIO`/`ANYHTTP_COROSIO`,
  `asio/`/`corosio/`, `build-corosio/`. "capy" names only the library itself (`capy::task`,
  `<boost/capy/...>`), the non-I/O base that corosio adds I/O to.
- Where the two styles differ by more than a few lines, each gets a file of its own, of the same
  name in `asio/` and `corosio/`, with the same definitions in the same order, so that the pair can
  be read side by side: `runtime.hpp`, `net.hpp`, `src/*/net.cpp`. What both share stays in the
  generic file of that name (`anyhttp/runtime.hpp`, which selects one of the two, `anyhttp/net.hpp`,
  `src/net.cpp`). The test fixtures follow the old pattern, `test/test_fixtures_*.hpp`. Keep the
  order when adding to either.
- The style comes from the generated `anyhttp/config.hpp` (`cmake/config.hpp.in`), not from a
  compile definition: a file that tests `ANYHTTP_ASIO`/`ANYHTTP_COROSIO` must include it (or
  `anyhttp/runtime.hpp`) first, or the test is silently false.
- Test `.cpp` files put `using namespace testing;` after the includes and use `HasSubstr`,
  `Values`, `Not` unqualified. Never in `test_fixtures.hpp` (it would leak), and not in
  `test_external.cpp`, whose own `Args` alias collides with gmock's.
- Commit subjects are prefixed: `fix:`, `refactor:`, `docs:`, `chore:`, `style:`, `test:`, or
  the protocol (`h1:`, `h2:`, `h3:`).

## Traps

**`ninja -n` lies here.** `src/CMakeLists.txt` globs with `CONFIGURE_DEPENDS`, so every invocation
starts with a CMake regen edge; in dry-run mode ninja stops there and prints nothing about the
compiles that would follow, which is indistinguishable from "nothing to do". Run the real build,
or query the stored dep database with `ninja -C build -t deps <object>`.

**Restart the server after regenerating the PKI.** Any build that touches `pki/*.json` wipes
`pki/out` including the root CA. A Server reads its certificate chain and key once, when it is
constructed, for TCP and QUIC alike, and keeps serving the old ones. curl then fails with
`verify result: 20`; the HTTP/3 client (which reads its CA file on every connect) with "unable to
get local issuer certificate".

**One TLS library per process.** By default anyhttp links AWS-LC (statically, from
`/opt/boringssl`, what `find_package(ssl CONFIG)` provides) together with
`ngtcp2_crypto_boringssl`. `-DTLS_LIBRARY=OpenSSL` switches to the system OpenSSL (3.5+) and
`ngtcp2_crypto_ossl`; `build-openssl/` is that tree. The code picks its variant from the TLS
headers (`OPENSSL_IS_AWSLC` / `OPENSSL_IS_BORINGSSL`), and the `#if`s are confined to
`Http3Session::configure_tls_context()` / `setup_tls()` and `tls.cpp` -- keep it that way.
After adding a dependency, check `ldd` of an AWS-LC build shows no `libssl.so.3` /
`libcrypto.so.3` -- a shared OpenSSL would interpose the executable's AWS-LC symbols. `curl`,
`osslclient` and `osslserver` are OpenSSL builds and are useful for interop testing.

**A capy context has to run dry.** Destroying a corosio `io_context`, or stopping it for good,
while a coroutine is suspended on it leaks that coroutine's whole stack: unlike ASIO, capy cannot
unwind it. LeakSanitizer reports it in `build-corosio-asan/`. Tests run their servers and clients to
their end -- `server.reset()` on the server's own thread, then let `run()` return.

**Benchmarking.** Confirm `UDP listening` appears in the server log before starting a load run: a
failed `bind()` aborts quietly and h2load will happily measure whatever other server owns the
port. Use ports from 18080 upwards. Upload benchmarks must target `/upload` (drains, then
responds), not `/eat_request` -- h2load stops sending the body once the response is complete.
[docs/benchmark.md](docs/benchmark.md) has the method, the tools that work on this host, and what
has been measured so far.
