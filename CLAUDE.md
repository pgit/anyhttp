# Working on anyhttp

C++23 HTTP/1.1 + HTTP/2 + HTTP/3 server and client on top of Boost.Asio. [README.md](README.md)
describes the API, the concurrency model and the reasoning behind it -- read it before changing
anything about request/response semantics. This file is about *how to work in the tree*.

## Build and test

Out-of-source trees, one per configuration. `build/` is Debug, `build-release/` is Release;
benchmark with the latter, never with `build/`. Both use clang, which builds against libc++ and
the libraries in `/opt/libc++` (`USE_LIBCXX`, `LIBCXX_ROOT`); `build-gcc/` is GCC with libstdc++
and `/usr/local`. Keep `build/` on clang: clangd reads its `compile_commands.json`.

`build-capy/` is the capy/corosio API style (`-DANYHTTP_API=CAPY -DTLS_LIBRARY=OpenSSL`, see
[docs/capy-port-plan.md](docs/capy-port-plan.md)); capy and corosio are FetchContent'd at pinned
SHAs. Both styles build the same library sources and test files; what belongs to one style only
is guarded with `ANYHTTP_CAPY`. The `server` and `client` programs are ASIO-only so far.
`build-capy-asan/` is its ASAN tree.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
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

Each protocol lives in files prefixed `h1_` (Beast/HTTP/1.1), `h2_` (nghttp2) and `h3_`
(ngtcp2+nghttp3), in both `include/anyhttp/` and `src/`. **Only `h2_*` files may include
`<nghttp2/*>`, only `h3_*` files `<ngtcp2/*>`/`<nghttp3/*>`.**

Generic code (`server_impl.*`, `client_impl.*`, `formatter.hpp`) reaches a backend only through
`h1_backend.hpp` / `h2_backend.hpp` / `h3_backend.hpp`, which declare their entry points in terms
of Asio types. New protocol-specific code goes in a prefixed file; when generic code needs to
reach it, add a declaration to that backend header rather than including a protocol header.

Verify the boundary with:

```
ninja -C build -t deps | awk '/^[^ ].*: #deps/{o=$1} /nghttp2\/nghttp2.h|ngtcp2\/ngtcp2.h/{print o}' | sort -u
```

Only `h2_*` and `h3_*` objects of the library may appear. (Test objects may: the h2c upgrade and
formatter tests drive nghttp2 by hand.)

The HTTP/3 server and client share one implementation: `h3_session.*` (all ngtcp2/nghttp3
callbacks, packet writing, timers, flow control), `h3_stream.*` (read and write paths, header
parsing, lifecycle, the reader/writer adapters), `h3_common.*` (helpers). `h3_server.cpp` and
`h3_client.cpp` hold only what is genuinely role-specific. Fix shared behavior in the shared
files.

## Conventions

- clang-format is authoritative: 3-space indent, 100 columns.
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
unwind it. LeakSanitizer reports it in `build-capy-asan/`. Tests run their servers and clients to
their end -- `server.reset()` on the server's own thread, then let `run()` return.

**Benchmarking.** Confirm `UDP listening` appears in the server log before starting a load run: a
failed `bind()` aborts quietly and h2load will happily measure whatever other server owns the
port. Use ports from 18080 upwards. Upload benchmarks must target `/upload` (drains, then
responds), not `/eat_request` -- h2load stops sending the body once the response is complete.
