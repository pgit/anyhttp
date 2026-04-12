# AnyHTTP Copilot Instructions

AnyHTTP is a type-erased interface for implementing asynchronous HTTP servers and clients. It exposes a common API for HTTP/1.1 and HTTP/2 today, with HTTP/3/QUIC work present in the tree but not wired through the public API yet.

## Working environment

Use the VS Code devcontainer in `.devcontainer/` for build and test work. The project depends on container-provisioned libraries and tools such as nghttp2/nghttp3/ngtcp2, aws-lc, spdlog, `boost_capy`, and `boost_corosio`, and the editor configuration expects `build/compile_commands.json`.

## Build and test commands

Run these inside the devcontainer.

```bash
# Configure (matches CI)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build
cmake --build build --parallel

# Run the full test suite
build/test/test_all
# or
ctest --test-dir build --output-on-failure

# Run a single discovered test with CTest
ctest --test-dir build -R '^FormatterTest\.ThreadId$' --output-on-failure

# Run a single gtest directly
build/test/test_all --gtest_filter='ClientConnect.WHEN_wrong_port_THEN_completes_with_host_not_found_eventually'
```

CI also exercises these configurations:

```bash
# Address sanitizer
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build --parallel
ASAN_OPTIONS=detect_leaks=1 build/test/test_all

# Thread sanitizer
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build --parallel
TSAN_OPTIONS=report_signal_unsafe=0 build/test/test_all

# Coverage
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build --parallel
cmake --build build --target coverage
```

There is no dedicated lint target in CMake. Formatting and static analysis are driven by `.clang-format`, `.clang-tidy`, and the devcontainer's clangd settings (`--clang-tidy`, `--compile-commands-dir=build`).

## High-level architecture

- Public API: `include/anyhttp/client.hpp`, `server.hpp`, and `session.hpp` expose the user-facing client/server/session types. They follow the Boost.Asio async model and use `asio::any_completion_handler` plus completion-token-based wrappers so the same operations work with coroutines or callbacks.
- Type erasure boundary: `include/anyhttp/common.hpp`, `client_impl.hpp`, `server_impl.hpp`, and `session_impl.hpp` define the internal `Reader`, `Writer`, and `Session::Impl` contracts. The public wrappers are thin owners around these impl objects.
- Protocol backends: HTTP/1.1 lives behind the Beast-based session code in `include/anyhttp/beast_session.hpp` and `src/beast_session.cpp`. HTTP/2 lives behind the nghttp2-based session/stream code in `include/anyhttp/nghttp2_session.hpp`, `include/anyhttp/nghttp2_stream.hpp`, `src/nghttp2_session.cpp`, and `src/nghttp2_stream.cpp`.
- Client protocol selection: `src/client_impl.cpp` resolves the remote endpoint, opens a TCP connection, and then chooses the backend from `client::Config.protocol`. `Protocol::h3` is currently rejected rather than partially implemented.
- Server protocol selection: `src/server_impl.cpp` accepts TCP connections, detects TLS, performs ALPN for `h2` vs `http/1.1`, checks for the HTTP/2 client preface on cleartext connections, and only then instantiates the matching session backend. Falling back to HTTP/1.1 is part of the intended control flow.
- Session lifetime: once a backend session is created, its `do_session()` coroutine owns the protocol-specific receive/send loop. The server tracks active sessions in `Server::Impl` so shutdown can close the accept loop and then destroy in-flight sessions.
- Reusable request behavior: `include/anyhttp/request_handlers.hpp` and `src/request_handlers.cpp` hold reusable coroutine handlers such as `echo`, `dump`, `eat_request`, `detach`, and `h2spec`. Both `src/server_main.cpp` and the integration-style tests wire server behavior together from these helpers.

## Key conventions

- Keep public APIs type-erased. New protocol-specific behavior should usually go behind `Impl` classes and backend session/stream types instead of adding backend details to `client::Request`, `client::Response`, `server::Request`, `server::Response`, or `Session`.
- Preserve Asio semantics when adding operations: bind associated executors where needed, forward cancellation slots when bridging into `co_spawn`, and keep completion-token overloads working instead of adding coroutine-only paths.
- Lifetime handling is explicit. Wrapper destructors call `destroy()`, and backend reader/writer objects use `detach()` when the owning session goes away. Changes around teardown, reset, or pending operations need to respect that pattern.
- Prefer coroutine request handlers via `Server::setRequestHandlerCoro()` and the existing helpers in `request_handlers.cpp`. The test suite is built around those helpers and around real client/server interaction rather than isolated unit seams.
- Configure in `build/`, not another build directory, when working in VS Code. The devcontainer's clangd and test-adapter settings point at `build/compile_commands.json` and `build/test/test_*`.
- The project assumes the container-provided toolchain and libraries: CMake forces `-stdlib=libc++`, includes range-v3 from `/usr/local/include/range-v3`, links nghttp2 through pkg-config, and expects the custom dependencies installed by the devcontainer image.
- Tests in `test/test_server.cpp` are the main behavioral spec. They cover both `Protocol::http11` and `Protocol::h2`, exercise cancellation and teardown paths, and use external tools such as `curl`, `nghttp`, and `h2load` when present in the container.
