# Working on anyhttp

C++23 HTTP/1.1 + HTTP/2 + HTTP/3 server and client on top of Boost.Asio. [docs/](docs/README.md)
has the API, the concurrency model and the reasoning behind it, in
[programmers-guide.rst](docs/programmers-guide.rst), [end-of-body.rst](docs/end-of-body.rst) and
[concurrent-requests.rst](docs/concurrent-requests.rst) -- read them before changing anything about
request/response semantics. This file is about *how to work in the tree*.

## Build and test

Out-of-source trees, one per configuration. `build/` is Debug, `build-release/` is Release;
benchmark with the latter, never with `build/`. Both use clang, which builds against libc++ and
the libraries in `/opt/libc++` (`USE_LIBCXX`, `LIBCXX_ROOT`); `build-gcc/` is GCC with libstdc++
and `/usr/local`. Keep `build/` on clang: clangd reads its `compile_commands.json`.

`build-corosio/` is the COROSIO I/O runtime (`-DANYHTTP_API=COROSIO -DTLS_LIBRARY=OpenSSL`, see
[docs/corosio-port-plan.md](docs/corosio-port-plan.md)); capy and corosio are FetchContent'd at
pinned SHAs. Both runtimes build the same library sources and test files; what belongs to one
runtime only is guarded with `ANYHTTP_ASIO` or `ANYHTTP_COROSIO` (exactly one of them is 1). So is the
`server` program; the `client` program is ASIO-only so far. `build-corosio-asan/` is its ASAN
tree, `build-corosio-release/` its Release tree (benchmark COROSIO with it) and
`build-corosio-gcc/` its GCC one -- capy and corosio do build against libstdc++, but GCC is
pickier about coroutine expressions than clang, so keep a `co_await` out of any larger expression.
COROSIO needs OpenSSL, so compare it against `build-openssl-release/` (ASIO, OpenSSL, Release),
not `build-release/`: `scripts/bench.sh` runs the two side by side (`-P` for plaintext, `-t` for
server threads).

`build-corosio-tsan/` is COROSIO under TSAN with `-DMULTITHREADED` in `CMAKE_CXX_FLAGS`: the
fixtures run the tests on all cores, a strand per connection. Expect reports inside corosio's
epoll reactor, which frees a socket's descriptor state while another thread still handles an
event for it (see the port plan, step 8); a report with an anyhttp frame at the racing access is
ours. A few tests set up their own server or client on the bare context and are not
thread-aware.

`build-h2v2/` serves HTTP/2 over nghttp2 version 2 (`-DH2_LIBRARY=nghttp2v2`), the rewrite, instead
of version 1 -- a build-time choice because the two cannot share a process, see the trap below.
Server and client both, so the whole suite runs: the expectations that differ by version are in
`test_external.cpp` (h2spec scores 135 of 146 instead of 138, three upstream differences named
there, and `curl_h2c_upgrade` is answered as HTTP/1.1 because version 2 dropped the h2c upgrade with
RFC 7540). What `test/CMakeLists.txt` leaves out of that tree is the three tests that drive an
nghttp2 session by hand -- `test_alt_svc`, `test_connection_end`, `test_h2c_upgrade` -- which are
written against version 1's API and would not link.
`build-h2v2-release/` is its Release tree, on OpenSSL so that `scripts/bench.sh -2` can put its
HTTP/2 beside the other two servers' with the HTTP/2 library as the only difference.

`build-deploy/` is what gets installed: Release, on AWS-LC, without the tests and so without cfssl
and the test PKI. `scripts/check_package.sh` installs it, moves the result and runs the programs
from there, compiles [test/package/](test/package/) against the package and again with anyhttp as a
FetchContent subproject, in `build-package/`. Run it after changing anything in a `CMakeLists.txt`,
or what a public header includes: each tree here builds the library with the tree's own directory
flags, so a usage requirement that is missing from the `anyhttp` target shows up only out there.

Every tree is a preset of its own name in [CMakePresets.json](CMakePresets.json) -- that is where
the options of each one live, and the options of a new tree belong there rather than in a command
line. `cmake --list-presets` names them all. Do not configure a tree by hand: `cc` and `c++` are
GCC on this host, so a forgotten `-DCMAKE_CXX_COMPILER=clang++` silently gives a tree the wrong
compiler.

```
cmake --preset build            # or: scripts/configure.sh build
cmake --build --preset build    # or: cmake --build build
ctest --preset build            # or: build/test/test_all
```

`scripts/configure.sh` configures all of the trees, or the ones named, and builds them too with
`-b`; it reads the tree names from the presets, so it needs nothing when one is added.
`scripts/bench.sh` configures the trees it needs when they are missing.

**Run the tests from the repo root**, never from inside a build tree: the servers load their TLS
material and the clients their CA through relative paths (the defaults of
`server::Config::tls_certificate_chain` / `tls_private_key`, and `pki/out/root.pem`, which the
test fixtures set as `client::Config::tls_ca_file`). From the wrong working directory every test
fails in `SetUp()` with "use_certificate_chain_file: No such file or directory". `ctest` is the
exception that proves nothing: `gtest_discover_tests()` gives every test case the repo root as its
working directory, and `ctest --preset` wants to run there anyway, this file being there.

The full suite takes about 50s serially (h2spec and the HTTP/3 timing tests dominate). Anything
running much longer means a test is hanging; kill it and run that test alone.

For routine runs use `gtest-parallel build/test/test_all` (same working-directory rule): about 7s.
Every server binds an ephemeral port, so parallel processes do not collide. It prints only
failing tests and does not report skips, and since each test gets its own process it cannot
catch interference through process-wide state (statics, the logger) -- do a serial run before committing changes to shared or global state.

`ANYHTTP_TEST_SCHEME=https` makes the URL of every test of a client say so, and the same tests run
over TLS, the server telling it from cleartext on the same port. Do that as well after changing
anything on the client's TCP path. COROSIO fails five tests of HTTP/1.1 that way, for a reason of
corosio's, see `.github/workflows/release.yml` and step 8 of the port plan.

Parametrized tests are suffixed `/HTTP11`, `/HTTP2`, `/HTTP3` -- not h2/h3. `--gtest_filter` knows
only `*` and `?`, and a filter that matches nothing exits 0 with no output, which reads like a
pass: check for `[ OK ]` lines.

### Documentation

`cmake --build build --target docs` makes the site in `build/docs/html`; the pages are `docs/*.rst`,
the reference comes from the comments in the public headers. [docs/README.md](docs/README.md) has
how to write it. Warnings are errors in Doxygen and Sphinx, so changing a public header can fail this
target: run it, a new public symbol also needs an entry in `docs/reference.rst`. The code the guide
shows is `docs/snippets.cpp` and `docs/examples/`, compiled in every tree with
`ANYHTTP_BUILD_DOCS` and run by `test/test_docs.cpp`. Change it there, never in a page.

### Sanitizers

Separate trees; the sanitizer options in the Debug branch of `CMakeLists.txt` are commented out on
purpose, so use the tree of the sanitizer instead of editing them:

```
cmake --build --preset build-asan
build-asan/test/test_all
```

GTest is built from source with the tree's own flags (`test/CMakeLists.txt`), so it is
instrumented too and needs no `ASAN_OPTIONS`. The `Recursion` test skips itself under ASAN, so
expect 3 extra skips.

TSAN is `build-tsan/`, the same flags with `-fsanitize=thread`, and runs clean without
suppressions. AWS-LC is
uninstrumented, but linked statically into the executable, so `called_from_lib:` cannot match it:
should a report ever show *both* stacks inside the TLS library, suppress it with `race:` on the
AWS-LC function instead. One with an anyhttp frame near the racing access is real. libc++ (the
system's `libc++.so.1`) is uninstrumented too and keeps `weak_ptr`'s `__release_weak()` out of
line, so the last `weak_ptr` dropped on another thread than the object's destructor ran on shows as
a race on `operator delete` in `__on_zero_shared_weak`, anyhttp frames and all;
[test/tsan_libcxx.cpp](test/tsan_libcxx.cpp) gives `test_all` an instrumented copy.

UBSAN is `build-ubsan/`, with reports fatal, so that one fails the test it happens in. It runs clean
over cleartext and TLS. CI does not run it.

Anything touching lifetimes on the HTTP/3 write path belongs under ASAN before it is committed --
completions there run on acknowledgement, and the Release build hides the use-after-frees. The same
goes for `h2v2/`'s write path, for the same reason: it hands nghttp2 a pointer into the caller's
buffer (see `WriteMode`), and `build-h2v2-asan/` is the version 2 ASAN tree for it.

## Source layout

Each protocol lives in a directory of its own, `h1/` (Beast/HTTP/1.1), `h2/` (nghttp2), `quic/`
(ngtcp2) and `h3/` (nghttp3, on top of `quic/`), in both `include/anyhttp/` and `src/`. **Only
files in `h2/` may include `<nghttp2/*>`, only files in `h2v2/` `<nghttp2v2/*>`, only files in
`quic/` and `h3/` `<ngtcp2/*>`, only files in `h3/` `<nghttp3/*>`.** What belongs to one async
runtime only is in `asio/` or `corosio/`, again in both; the library compiles the `src/` directory
of its runtime and not the other. `src/apps/` has the `server` and `client` programs, `src/issues/` standalone
reproducers of upstream bugs. Everything else at the top level is generic.

What gets installed is the public headers, the ones reachable from `client.hpp`, `server.hpp`,
`quic.hpp`, `session.hpp` and the like; `cmake/install.cmake` leaves out the protocol directories,
`net.hpp` and `*_impl.hpp`. A public header that starts including one of those needs the list changed
-- and if that is a protocol header, more thought than that, because then every user of the library
needs that protocol library's headers. What a public header does need comes with the target, as
`PUBLIC` in `src/CMakeLists.txt`.

Generic code (`server_impl.*`, `client_impl.*`, `formatter.hpp`) reaches a backend only through
`h1/backend.hpp` / `h2/backend.hpp` / `quic/backend.hpp` / `h3/backend.hpp`, which declare their
entry points in terms
of the runtime layer. New protocol-specific code goes in that protocol's directory; when generic
code needs to reach it, add a declaration to that backend header rather than including a protocol
header.

`h2v2/` is the second HTTP/2 backend, nghttp2 version 2, an alternative to `h2/` rather than an
addition: exactly one of the two is compiled, picked by `H2_LIBRARY`, and both define the factories
`anyhttp/h2/backend.hpp` declares, so nothing above that header can tell them apart. Each file in
`h2v2/` has a counterpart of the same name in `h2/` with the same definitions in the same order,
the way `asio/` and `corosio/` do, so the pair can be read side by side.

What `h2v2/` has and `h2/` has no counterpart for is `WriteMode` (`h2v2/stream.hpp`), because
version 2 takes an outgoing body *by reference* where version 1 copied it: nghttp2 keeps reading
from the vector it was handed until it has framed all of it, which flow control can postpone
indefinitely. A response is handed over as `ZeroCopy`, straight out of the caller's buffer, and a
write cancelled with bytes still outstanding resets the stream -- there being no way to un-offer
that memory. A request body goes `Staged`, copied through a bounded buffer of the stream's, so that
cancelling a write leaves the stream usable and the body can still be ended afterwards. This is the
same split, for the same reason, as `quic::WriteMode` in HTTP/3; read the two together.

Verify the boundary with:

```
ninja -C build -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp2\/nghttp2.h|ngtcp2\/ngtcp2.h/{print o}' | sort -u
ninja -C build -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp3\/nghttp3.h/{print o}' | sort -u
```

In `build-h2v2/`, the first command must print nothing at all -- version 1's header has no business
there -- and version 2's belongs only to `h2v2/` (plus the formatter test):

```
ninja -C build-h2v2 -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp2\/nghttp2.h/{print o}' | sort -u
ninja -C build-h2v2 -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp2v2\/nghttp2.h/{print o}' | sort -u
```

Only objects in `h2/`, `quic/` and `h3/` of the library may appear in the first list, only objects
in `h3/` in the second: that is what proves the QUIC transport has no HTTP in it. (Test objects
may: the h2c upgrade and formatter tests drive nghttp2 by hand.)

**Generic and protocol code include nothing from Boost.Asio but `<boost/asio/buffer.hpp>`**, which
Beast's parser, serializer and `Fields` bring anyway, and nothing from capy or corosio. They reach
the runtime through `anyhttp/runtime.hpp`, `anyhttp/net.hpp` and `anyhttp/formatter.hpp`, which
include the `asio/` or `corosio/` header of the build's runtime. Addresses and endpoints are the
runtime's own (`IpAddress`, `TcpEndpoint`, `UdpEndpoint`); where the two differ, `net.hpp` declares
a function each runtime defines (`io::make_address()`, `io::to_sockaddr()`, ...). Verify in a
COROSIO tree that no library object includes more of Asio:

```
ninja -C build-corosio -t deps | awk '/^[^ ].*: #deps/{o=$1} /boost\/asio\// && !/boost\/asio\/((buffer|is_contiguous_iterator|version)\.hpp|detail\/)/{print o}' | grep anyhttp.dir | sort -u
```

It must print nothing. (Test objects may: `test_external.cpp` runs its child processes on an ASIO
context on both runtimes.)

QUIC and HTTP/3 are two layers, and each shares one implementation between server and client.
`quic/session.*` has all ngtcp2 callbacks, packet writing, timers and connection flow control,
`quic/stream.*` what every stream has, `quic/common.*` helpers; `quic/server.cpp` (UDP socket, CID
demux, closing period) and `quic/client.cpp` (connected socket) hold only what is genuinely
role-specific. What runs on a connection is a `QuicApplication` (`quic/application.hpp`), picked
by ALPN. HTTP/3 is one: `h3/application.*` (all nghttp3 callbacks, nghttp3 as the write
scheduler), `h3/stream.*` (header parsing, the nghttp3 data reader, the reader/writer adapters'
header section), and again `h3/server.cpp` and `h3/client.cpp` for the roles. Plain QUIC streams
(public API `anyhttp/quic.hpp`, tests `test_quic.cpp`) are the other: `quic/streams.*` (the
application, with its round-robin write scheduler) and `quic/connection.cpp` (the handles). The
read and write paths and the lifecycle of both are `QuicStream`'s. Fix shared behavior in the
shared files.

## Conventions

- clang-format is authoritative: 3-space indent, 100 columns.
- The two I/O runtimes are ASIO and COROSIO: `ANYHTTP_ASIO`/`ANYHTTP_COROSIO`,
  `asio/`/`corosio/`, `build-corosio/`. "capy" names only the library itself (`capy::task`,
  `<boost/capy/...>`), the non-I/O base that corosio adds I/O to.
- Where the two runtimes differ by more than a few lines, each gets a file of its own, of the same
  name in `asio/` and `corosio/`, with the same definitions in the same order, so that the pair can
  be read side by side: `runtime.hpp`, `net.hpp`, `src/*/net.cpp`. What both share stays in the
  generic file of that name (`anyhttp/runtime.hpp`, which selects one of the two, `anyhttp/net.hpp`,
  `src/net.cpp`). The test fixtures follow the old pattern, `test/test_fixtures_*.hpp`. Keep the
  order when adding to either.
- The runtime comes from the generated `anyhttp/config.hpp` (`cmake/config.hpp.in`), not from a
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

**One nghttp2 per process.** Version 2 installs as its own package (`<nghttp2v2/nghttp2.h>`,
`libnghttp2v2`), but still exports nine of version 1's symbol names -- `nghttp2_strerror`,
`nghttp2_http2_strerror`, `nghttp2_check_header_name`/`_value`, `nghttp2_check_authority` and the
four `nghttp2_rcbuf_*`, whose struct layouts differ between the two. So a binary links one or the
other, never both, which is why `H2_LIBRARY` is a build-time choice and `h2v2/` a separate
directory rather than a second backend alongside `h2/`. Both headers also use the `NGHTTP2_H`
include guard, so a translation unit that includes both silently sees only the first.

**One TLS library per process.** By default anyhttp links AWS-LC (statically, from
`/opt/boringssl`, what `find_package(ssl CONFIG)` provides) together with
`ngtcp2_crypto_boringssl`. `-DTLS_LIBRARY=OpenSSL` switches to the system OpenSSL (3.5+) and
`ngtcp2_crypto_ossl`; `build-openssl/` is that tree. The code picks its variant from the TLS
headers (`OPENSSL_IS_AWSLC` / `OPENSSL_IS_BORINGSSL`), and the `#if`s are confined to
`QuicSession::configure_tls_context()` / `setup_tls()` and `tls.cpp` -- keep it that way.
After adding a dependency, check `ldd` of an AWS-LC build shows no `libssl.so.3` /
`libcrypto.so.3` -- a shared OpenSSL would interpose the executable's AWS-LC symbols. `curl`,
`osslclient` and `osslserver` are OpenSSL builds and are useful for interop testing.

**A context has to run dry, on both runtimes.** This is anyhttp's doing, not capy's any more. Since
capy `8e1244f` and corosio `232c79f`, destroying an `io_context` unwinds the coroutines suspended on
it, provided the continuation they are parked with carries `env->owner`: whatever in
`corosio/runtime.hpp` parks or queues a coroutine has to set it. What that cannot reach is a cycle
through an object that only comes apart by running. `QuicServerSession::do_session()` waits on an
event of the session that its own completion handler keeps alive, and a client frame that holds its
`Session` or `Request` holds the `Completion` it is waiting on. HTTP/2 and HTTP/3 leak without the
drain on ASIO as well; HTTP/1.1 does not. LeakSanitizer does not see all of it, because capy pools
frames below 2 KiB: count destructors to be sure. Tests run their servers and clients to their end
-- `server.reset()` on the server's own thread, then let `run()` return.

**Benchmarking.** Confirm `UDP listening` appears in the server log before starting a load run: a
failed `bind()` aborts quietly and h2load will happily measure whatever other server owns the
port. Use ports from 18080 upwards. Upload benchmarks must target `/upload` (drains, then
responds), not `/eat_request` -- h2load stops sending the body once the response is complete.
[docs/benchmark.md](docs/benchmark.md) has the method, the tools that work on this host, and what
has been measured so far.
