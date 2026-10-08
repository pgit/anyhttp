#!/usr/bin/env bash
#
# Configures the out-of-source build trees described in CLAUDE.md: all of them, or the ones named.
# Configuring an existing tree again is harmless; to change its compiler, delete it first.
#
#   build                  clang, libc++, AWS-LC, Debug (clangd reads its compile_commands.json)
#   build-release          the same, Release
#   build-gcc              GCC, libstdc++, AWS-LC, Debug
#   build-asan             clang, AWS-LC, RelWithDebInfo, AddressSanitizer
#   build-tsan             clang, AWS-LC, RelWithDebInfo, ThreadSanitizer
#   build-openssl          clang, OpenSSL, Debug
#   build-openssl-release  clang, OpenSSL, Release: the ASIO tree bench.sh compares COROSIO with
#   build-corosio          COROSIO (clang, OpenSSL), Debug
#   build-corosio-release  COROSIO, Release
#   build-corosio-asan     COROSIO, RelWithDebInfo, AddressSanitizer
#   build-corosio-tsan     COROSIO, RelWithDebInfo, ThreadSanitizer, tests on all cores
#
# Usage: scripts/configure.sh [-b] [tree...]
#
#   -b  build the trees too
#
# Run from anywhere; the trees are created in the repo root.
#
set -euo pipefail

build=0

while getopts "bh" opt; do
   case $opt in
   b) build=1 ;;
   *) sed -n '/^set /q; 3,$p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
   esac
done
shift $((OPTIND - 1))

cd "$(dirname "$0")/.."

all=(build build-release build-gcc build-asan build-tsan build-openssl build-openssl-release
   build-corosio build-corosio-release build-corosio-asan build-corosio-tsan)

# cc and c++ are GCC on this host
clang=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
openssl=(-DTLS_LIBRARY=OpenSSL)
corosio=(-DANYHTTP_API=COROSIO -DTLS_LIBRARY=OpenSSL)

# Appends to args a RelWithDebInfo build with sanitizer $1 (address or thread) and the further
# compiler flags $2...
sanitize() {
   args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo
      "-DCMAKE_CXX_FLAGS=-fsanitize=$1 -fno-omit-frame-pointer -g${2:+ ${*:2}}"
      "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=$1")
}

# Sets args to the CMake options of tree $1.
options() {
   case $1 in
   build) args=("${clang[@]}" -DCMAKE_BUILD_TYPE=Debug) ;;
   build-release) args=("${clang[@]}" -DCMAKE_BUILD_TYPE=Release) ;;
   build-gcc) args=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug) ;;
   build-asan) args=("${clang[@]}") && sanitize address ;;
   build-tsan) args=("${clang[@]}") && sanitize thread ;;
   build-openssl) args=("${clang[@]}" "${openssl[@]}" -DCMAKE_BUILD_TYPE=Debug) ;;
   build-openssl-release) args=("${clang[@]}" "${openssl[@]}" -DCMAKE_BUILD_TYPE=Release) ;;
   build-corosio) args=("${clang[@]}" "${corosio[@]}" -DCMAKE_BUILD_TYPE=Debug) ;;
   build-corosio-release) args=("${clang[@]}" "${corosio[@]}" -DCMAKE_BUILD_TYPE=Release) ;;
   build-corosio-asan) args=("${clang[@]}" "${corosio[@]}") && sanitize address ;;
   build-corosio-tsan) args=("${clang[@]}" "${corosio[@]}") && sanitize thread -DMULTITHREADED ;;
   *)
      echo "error: unknown tree '$1', one of: ${all[*]}" >&2
      exit 2
      ;;
   esac
}

(($#)) || set -- "${all[@]}"
trees=("${@%/}")
for tree in "${trees[@]}"; do
   options "$tree"
done

for tree in "${trees[@]}"; do
   options "$tree"
   echo "=== $tree"
   cmake -S . -B "$tree" -G Ninja "${args[@]}"
done

if ((build)); then
   for tree in "${trees[@]}"; do
      echo "=== building $tree"
      cmake --build "$tree"
   done
fi
