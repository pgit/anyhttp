# Development container

anyhttp is developed and tested in a devcontainer. The same image runs the CI jobs in
[`.github/workflows/release.yml`](../.github/workflows/release.yml), so a build that works in the
container is the build CI does.

There are three image layers. Only the last one is built locally when you open the project:

| Image                     | Built from                                                         | Contents                                   |
| ------------------------- | ------------------------------------------------------------------ | ------------------------------------------ |
| `psedoc/cpp-devcontainer` | [pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)  | generic C++ toolchain and libraries        |
| `psedoc/anyhttp`          | [`.devcontainer/base/Dockerfile`](../.devcontainer/base/Dockerfile) | the HTTP/2 and HTTP/3 stack anyhttp needs  |
| (local, per checkout)     | [`.devcontainer/Dockerfile`](../.devcontainer/Dockerfile)           | interactive tools, shell settings          |

`psedoc/anyhttp` changes rarely and takes a while to build, so it is built once and pushed to
Docker Hub. Opening the project (or a codespace) then only pulls it and adds the thin top layer.

## What cpp-devcontainer provides

See [its repository](https://github.com/pgit/cpp-devcontainer) for details. In short: a Debian
trixie image with GCC, a current clang from apt.llvm.org, CMake, Ninja, gdb, and
`gtest-parallel`. The C++ libraries are built twice, once for each standard library, because a
C++ library built against libstdc++ cannot be linked into a libc++ program or vice versa:

| Prefix        | Standard library | Libraries                       |
| ------------- | ---------------- | ------------------------------- |
| `/usr/local`  | libstdc++ (GCC)  | Boost, GTest/GMock, fmt, spdlog |
| `/opt/libc++` | libc++ (clang)   | the same set                    |

With clang, anyhttp builds against libc++ and must pick up the libraries from `/opt/libc++`; with
GCC it uses libstdc++ and the ones in `/usr/local`. `USE_LIBCXX` in the top level
[`CMakeLists.txt`](../CMakeLists.txt) selects which (default: on for clang), and `LIBCXX_ROOT`
(default: `/opt/libc++`) where the libc++ builds are.

## What anyhttp adds

[`.devcontainer/base/Dockerfile`](../.devcontainer/base/Dockerfile) adds the protocol stack. All of
it is C, so a single build in `/usr/local` serves both standard libraries.

- **AWS-LC** in `/opt/boringssl`, static libraries only. It is API-compatible with BoringSSL,
  which is what ngtcp2 calls it. It is deliberately kept out of `/usr/local` so it does not
  shadow the system OpenSSL; see "One TLS library per process" in [CLAUDE.md](../CLAUDE.md).
- **nghttp3** (lib only) in `/usr/local`.
- **ngtcp2** in `/usr/local`, with both crypto backends: `ngtcp2_crypto_boringssl` (what anyhttp
  links by default) and `ngtcp2_crypto_ossl` (with `-DTLS_LIBRARY=OpenSSL`). Its OpenSSL example programs are installed as `osslclient`
  and `osslserver` for interop tests.
- **urlparse** (from ngtcp2's third-party tree).
- **nghttp2** with HTTP/3 support, built against the OpenSSL variant: the library for the HTTP/2
  backend, plus `nghttp`, `nghttpd`, `nghttpx` and `h2load` for testing and benchmarking.
  **libbpf** is built first so that `nghttpx` can use it for QUIC connection steering.
- **curl** from git master with nghttp2, ngtcp2 and nghttp3 over OpenSSL, so `curl --http3`
  works. The apt package is removed so the tests cannot pick up an older curl.
- `libev` (needed by the nghttp2 and ngtcp2 tools) and `golang-cfssl` (used to generate the
  test PKI in `pki/`).
- The Debian `libnghttp2-dev` / `libnghttp3-dev` packages are removed, so CMake and pkg-config
  cannot find a stale system copy instead of the one in `/usr/local`.
- gdb helpers for the `vscode` user: libc++ pretty printers and Asio's debugger extensions
  (backtraces across `co_await`), both registered in `~/.gdbinit`.

The top layer, [`.devcontainer/Dockerfile`](../.devcontainer/Dockerfile), adds only things for
interactive use: `tmux`, `btop`, `valgrind`, `ping`, persistent bash history (a volume mounted at
`/commandhistory`), and the aliases in `.devcontainer/.bash_aliases`.
[`devcontainer.json`](../.devcontainer/devcontainer.json) adds the VS Code extensions (clangd,
CMake Tools, TestMate, CodeLLDB, ...), `SYS_PTRACE` for debuggers and sanitizers, and bind-mounts
`~/.claude` from the host.

## Updating the images

Bump a dependency by changing its `ARG ..._VERSION` in the base Dockerfile, then:

```
cd .devcontainer/base
docker build -t psedoc/anyhttp:testing .
```

`.devcontainer/Dockerfile` builds `FROM` that tag, so rebuilding the devcontainer ("Dev
Containers: Rebuild Container" in VS Code) picks it up. Once the tests pass in it, tag and push a
numbered version and point both `.devcontainer/Dockerfile` and the `image:` entries in
`.github/workflows/release.yml` at it. Keep the `psedoc/anyhttp` and `psedoc/cpp-devcontainer`
version numbers in step, so it is clear which base a given anyhttp image was built on.
