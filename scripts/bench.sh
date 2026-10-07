#!/usr/bin/env bash
#
# Runs h2load against the standalone server of the ASIO and the COROSIO release builds, over
# HTTP/1.1, HTTP/2 and HTTP/3 (all TLS), and prints a summary table. Both builds use OpenSSL, as
# COROSIO has to, so that the TLS library is not part of the comparison: build-openssl-release/
# is the ASIO one (build-release/ is ASIO on AWS-LC).
#
# nghttpd, if it is on the PATH, runs the HTTP/2 rows as a reference: the same nghttp2, on OpenSSL
# as well, serving files. Its document root has "Hello, World!" for / and the repo's test/ for
# /test/*, like the server program; any other path is a 404 there, counted as failed. With -t it
# gets as many workers, an event loop each, to which its accepting thread hands the connections
# round robin -- closer to -i than to one shared context.
#
# Usage: scripts/bench.sh [-D seconds] [-c clients] [-m streams] [-M depth] [-t threads] [-i]
#                         [-p port] [-u path] [-P] [-n] [-N] [-v]
#
#   -D  duration of each run in seconds (default 10)
#   -c  number of h2load clients (default 8)
#   -m  max concurrent streams per client for h2 and h3 (default 10)
#   -M  h1 pipelining: requests in flight per connection (default 1, no pipelining)
#   -t  threads the servers run on (default 1); h2load gets as many, up to the clients
#   -i  independent: an I/O context and a server per thread, sharing the port (SO_REUSEPORT),
#       instead of one context on all threads with a strand per connection
#   -p  port (default 18080)
#   -u  request path (default /)
#   -P  plaintext: HTTP/1.1 and HTTP/2 (prior knowledge) without TLS; no HTTP/3
#   -n  don't build the servers first
#   -N  leave out nghttpd
#   -v  print the command line that starts each server
#
# Run from anywhere; the servers are started in the repo root so they find their TLS material.
# Raw h2load output and server logs are kept in a temporary directory, printed at the end.
#
set -euo pipefail

duration=10
clients=8
streams=10
pipeline=1
threads=1
port=18080
path=/
build=1
nghttpd=1
verbose=0
scheme=https
independent=()

while getopts "D:c:m:M:t:ip:u:PnNvh" opt; do
   case $opt in
   D) duration=$OPTARG ;;
   c) clients=$OPTARG ;;
   m) streams=$OPTARG ;;
   M) pipeline=$OPTARG ;;
   t) threads=$OPTARG ;;
   i) independent=(--independent) ;;
   p) port=$OPTARG ;;
   u) path=$OPTARG ;;
   P) scheme=http ;;
   n) build=0 ;;
   N) nghttpd=0 ;;
   v) verbose=1 ;;
   *) sed -n '/^set /q; 3,$p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
   esac
done

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

declare -A trees=([asio]=build-openssl-release [corosio]=build-corosio-release)
styles=(asio corosio)
protocols=(h1 h2 h3)
[[ $scheme == http ]] && protocols=(h1 h2)
servers=("${styles[@]}")
if ((nghttpd)); then
   if command -v nghttpd >/dev/null; then
      servers+=(nghttpd)
   else
      echo "warning: nghttpd is not on the PATH, leaving it out" >&2
   fi
fi

if ((build)); then
   for style in "${styles[@]}"; do
      cmake --build "${trees[$style]}" --target server >/dev/null
   done
fi

out=$(mktemp -d -t anyhttp-bench.XXXXXX)
server_pid=

# nghttpd's document root: what the server program answers on / and /test/*
docroot=$out/htdocs
mkdir "$docroot"
printf 'Hello, World!\n' >"$docroot/index.html"
ln -s "$root/test" "$docroot/test"

stop_server() {
   if [[ -n $server_pid ]]; then
      kill -TERM "$server_pid" 2>/dev/null || true
      wait "$server_pid" 2>/dev/null || true
      server_pid=
   fi
}
trap stop_server EXIT

# Starts server $1 (a style or nghttpd) and waits until it listens: on TCP and UDP (on every thread
# with -i), nghttpd on TCP. A failed bind() aborts quietly, and h2load would then measure whatever
# else owns the port.
start_server() {
   local server=$1 log=$out/server-$1.log
   # With -i the servers bind with SO_REUSEPORT, and so would share the port with a leftover one.
   if ss -Hltun "sport = :$port" | grep -q .; then
      echo "error: port $port is in use:" >&2
      ss -ltunp "sport = :$port" >&2
      exit 1
   fi
   local cmd
   if [[ $server == nghttpd ]]; then
      local listen=("$port" pki/out/server-key.pem pki/out/server-chain.pem)
      [[ $scheme == http ]] && listen=(--no-tls "$port")
      # 1 MiB windows like anyhttp; 100 concurrent streams is the default of both
      cmd=(nghttpd -n "$threads" -w 20 -W 20 -d "$docroot" "${listen[@]}")
   else
      cmd=("${trees[$server]}/src/server" -p "$port" -t "$threads" "${independent[@]}")
   fi
   if ((verbose)); then
      local line
      printf -v line '%q ' "${cmd[@]}"
      echo "\$ ${line% }" >&2
   fi
   "${cmd[@]}" >"$log" 2>&1 &
   server_pid=$!
   for _ in $(seq 50); do
      if [[ $server == nghttpd ]]; then
         # nghttpd logs nothing when it comes up
         ss -Hltnp "sport = :$port" | grep -q "pid=$server_pid," && return 0
      else
         (($(grep -c "UDP listening" "$log") == (${#independent[@]} ? threads : 1))) && return 0
      fi
      kill -0 "$server_pid" 2>/dev/null || break
      sleep 0.1
   done
   echo "error: $server server did not come up on port $port, see $log" >&2
   exit 1
}

# Prints "req/s MB/s succeeded failed median p99" (latencies per request) from an h2load output
# file.
parse() {
   awk '
      /^finished in/ { rps = $4; mbs = $6; sub(/,$/, "", mbs) }
      /^requests:/   { ok = $8; failed = $10 }
      /^request +:/  { median = $5; p99 = $7 }
      END { printf "%s %s %s %s %s %s\n", rps, mbs, ok, failed, median, p99 }' "$1"
}

# h2load wants at least one client per thread
h2load_threads=$((threads < clients ? threads : clients))

declare -A results
for server in "${servers[@]}"; do
   start_server "$server"
   for proto in "${protocols[@]}"; do
      [[ $server == nghttpd && $proto != h2 ]] && continue
      args=(-D "$duration" -c "$clients" -m "$streams" -t "$h2load_threads")
      case $proto in
      h1) args=(--h1 -D "$duration" -c "$clients" -m "$pipeline" -t "$h2load_threads") ;;
      h3) args+=(--h3) ;;
      esac
      log=$out/h2load-$server-$proto.log
      printf '%-8s %-3s ... ' "$server" "$proto" >&2
      if h2load "${args[@]}" "$scheme://127.0.0.1:$port$path" >"$log" 2>&1; then
         results[$server,$proto]=$(parse "$log")
         echo "$(cut -d' ' -f1 <<<"${results[$server,$proto]}") req/s" >&2
      else
         results[$server,$proto]="- - - - - -"
         echo "h2load failed, see $log" >&2
      fi
   done
   stop_server
done

echo
echo "h2load -D ${duration}s -c $clients -m $streams -t $h2load_threads (h1: -m $pipeline)," \
   "$scheme://127.0.0.1:$port$path, server threads: $threads${independent:+ (independent)}"
echo
row='%-8s %-5s %12s %10s %10s %7s %9s %9s\n'
printf "$row" server proto req/s MB/s succeeded failed median p99
printf "$row" -------- ----- ------------ ---------- ---------- ------- --------- ---------
for proto in "${protocols[@]}"; do
   for server in "${servers[@]}"; do
      [[ -v results[$server,$proto] ]] || continue
      read -r rps mbs ok failed median p99 <<<"${results[$server,$proto]}"
      printf "$row" "$server" "$proto" "$rps" "$mbs" "$ok" "$failed" "$median" "$p99"
   done
done

# corosio and nghttpd relative to asio, per protocol
echo
for proto in "${protocols[@]}"; do
   a=$(cut -d' ' -f1 <<<"${results[asio,$proto]}")
   for server in "${servers[@]:1}"; do
      [[ -v results[$server,$proto] ]] || continue
      b=$(cut -d' ' -f1 <<<"${results[$server,$proto]}")
      if [[ $a != - && $b != - ]]; then
         awk -v p="$proto" -v s="$server" -v a="$a" -v b="$b" \
            'BEGIN { printf "%s: %s/asio = %.2f\n", p, s, (a > 0 ? b / a : 0) }'
      fi
   done
done
echo
echo "logs: $out"
