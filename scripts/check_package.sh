#!/usr/bin/env bash
#
# Checks that anyhttp works from the outside, the way the README says to use it: installed, moved
# somewhere else and found by find_package(), and built along as a subproject by FetchContent. The
# consumer is test/package/, the work happens in build-package/.
#
# Usage: scripts/check_package.sh [check...]
#
#   install        the build-deploy tree installed and relocated: the programs run from there and
#                  serve every protocol, and a project compiles and runs against the package
#   embed-asio     FetchContent, on ASIO and OpenSSL
#   embed-corosio  FetchContent, on COROSIO, which needs OpenSSL as well
#
# Without any, all of them.
#
set -euo pipefail

self=$(realpath "$0")
cd "$(dirname "$self")/.."
source_dir=$PWD
work=$source_dir/build-package

compilers=(-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)

# the same self-signed certificate is the server's and the client's CA
certificate() {
   [[ -f $work/cert.pem ]] && return
   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 30 \
      -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 \
      -keyout "$work/key.pem" -out "$work/cert.pem" 2>/dev/null
}

run_consumer() {
   timeout -s KILL 30 "$1/consumer" "$work/cert.pem" "$work/key.pem"
}

# <expected version> <curl option...>: any status will do, as this is about the protocol
expect_version() {
   local expected=$1 got
   shift
   got=$(timeout -s KILL 10 curl -sS -o /dev/null -w '%{http_version}' "$@")
   echo "curl $*: HTTP/$got"
   [[ $got == "$expected" ]] || {
      echo "error: expected HTTP/$expected" >&2
      return 1
   }
}

check_programs() {
   local bin=$1/bin port=18180 log=$work/server.log pid
   local libs
   libs=$(ldd "$bin/anyhttp-server")
   echo "$libs"
   if grep -E '=> (/usr/local|/opt)|not found' <<<"$libs"; then
      echo "error: the program still depends on something outside of its tree" >&2
      return 1
   fi
   env -i "$bin/anyhttp-client" --help >/dev/null

   # a certificate that can't be read is an error message, not an abort
   local status=0
   timeout -s KILL 5 env -i "$bin/anyhttp-server" -p "$port" --cert "$work/missing.pem" \
      >/dev/null 2>&1 || status=$?
   [[ $status == 1 ]] || {
      echo "error: the server without its certificate exited with $status, not 1" >&2
      return 1
   }

   env -i "$bin/anyhttp-server" -p "$port" --cert "$work/cert.pem" --key "$work/key.pem" \
      >"$log" 2>&1 &
   pid=$!
   # shellcheck disable=SC2064 # the PID of now is the one to kill
   trap "kill $pid 2>/dev/null || true; trap - RETURN" RETURN
   for _ in {1..50}; do
      grep -q 'UDP listening' "$log" && break
      sleep 0.1
   done
   grep -q 'UDP listening' "$log" || {
      cat "$log"
      echo "error: the server did not come up" >&2
      return 1
   }

   local url=http://127.0.0.1:$port/ tls=https://127.0.0.1:$port/
   expect_version 1.1 --http1.1 "$url"
   expect_version 2 --http2-prior-knowledge "$url"
   expect_version 1.1 --cacert "$work/cert.pem" --http1.1 "$tls"
   expect_version 2 --cacert "$work/cert.pem" --http2 "$tls"
   expect_version 3 --cacert "$work/cert.pem" --http3-only "$tls"
}

check_install() {
   echo "=== install"
   cmake --preset build-deploy
   cmake --build --preset build-deploy

   rm -rf "$work/staging" "$work/moved" "$work/install"
   cmake --install build-deploy --prefix "$work/staging"
   mv "$work/staging" "$work/moved"
   local prefix=$work/moved

   # nothing of the protocol libraries' headers, and nothing of the other runtime
   if find "$prefix/include" | grep -E '/(h1|h2|h2v2|h3|quic|corosio)(/|$)|_impl\.hpp$|/net\.hpp$'; then
      echo "error: the headers above are not public" >&2
      return 1
   fi

   check_programs "$prefix"

   cmake -S test/package -B "$work/install" "${compilers[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$prefix"
   cmake --build "$work/install"
   run_consumer "$work/install"
}

check_embed() {
   local name=$1
   shift
   echo "=== $name"
   cmake -S test/package -B "$work/$name" "${compilers[@]}" -DCMAKE_BUILD_TYPE=Debug \
      -DANYHTTP_EMBED=ON -DFETCHCONTENT_SOURCE_DIR_ANYHTTP="$source_dir" "$@"
   cmake --build "$work/$name"
   run_consumer "$work/$name"
}

(($#)) || set -- install embed-asio embed-corosio

mkdir -p "$work"
certificate

for check in "$@"; do
   case $check in
   install) check_install ;;
   embed-asio) check_embed embed-asio -DTLS_LIBRARY=OpenSSL ;;
   embed-corosio) check_embed embed-corosio -DANYHTTP_API=COROSIO -DTLS_LIBRARY=OpenSSL ;;
   *)
      sed -n '/^set /q; 3,$p' "$self" | sed 's/^# \{0,1\}//'
      exit 2
      ;;
   esac
done
echo "all good"
