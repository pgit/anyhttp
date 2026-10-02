#!/usr/bin/env bash
#
# Runs h2load against the standalone server of the ASIO and the COROSIO release builds, over
# HTTP/1.1, HTTP/2 and HTTP/3 (all TLS), and prints a summary table.
#
# Usage: scripts/bench.sh [-D seconds] [-c clients] [-m streams] [-p port] [-u path] [-n]
#
#   -D  duration of each run in seconds (default 10)
#   -c  number of h2load clients (default 8)
#   -m  max concurrent streams per client for h2 and h3 (default 10); h1 always uses 1
#   -p  port (default 18080)
#   -u  request path (default /)
#   -n  don't build the servers first
#
# Run from anywhere; the servers are started in the repo root so they find their TLS material.
# Raw h2load output and server logs are kept in a temporary directory, printed at the end.
#
set -euo pipefail

duration=10
clients=8
streams=10
port=18080
path=/
build=1

while getopts "D:c:m:p:u:nh" opt; do
   case $opt in
   D) duration=$OPTARG ;;
   c) clients=$OPTARG ;;
   m) streams=$OPTARG ;;
   p) port=$OPTARG ;;
   u) path=$OPTARG ;;
   n) build=0 ;;
   *) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
   esac
done

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

declare -A trees=([asio]=build-release [corosio]=build-corosio-release)
styles=(asio corosio)
protocols=(h1 h2 h3)

if ((build)); then
   for style in "${styles[@]}"; do
      cmake --build "${trees[$style]}" --target server >/dev/null
   done
fi

out=$(mktemp -d -t anyhttp-bench.XXXXXX)
server_pid=

stop_server() {
   if [[ -n $server_pid ]]; then
      kill -TERM "$server_pid" 2>/dev/null || true
      wait "$server_pid" 2>/dev/null || true
      server_pid=
   fi
}
trap stop_server EXIT

# Starts the server of $1 and waits until it listens on TCP and UDP. A failed bind() aborts
# quietly, and h2load would then measure whatever else owns the port.
start_server() {
   local style=$1 log=$out/server-$1.log
   "${trees[$style]}/src/server" -p "$port" >"$log" 2>&1 &
   server_pid=$!
   for _ in $(seq 50); do
      grep -q "UDP listening" "$log" && return 0
      kill -0 "$server_pid" 2>/dev/null || break
      sleep 0.1
   done
   echo "error: $style server did not come up on port $port, see $log" >&2
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

declare -A results
for style in "${styles[@]}"; do
   start_server "$style"
   for proto in "${protocols[@]}"; do
      args=(-D "$duration" -c "$clients" -m "$streams")
      case $proto in
      h1) args=(--h1 -D "$duration" -c "$clients" -m 1) ;;
      h3) args+=(--h3) ;;
      esac
      log=$out/h2load-$style-$proto.log
      printf '%-8s %-3s ... ' "$style" "$proto" >&2
      if h2load "${args[@]}" "https://127.0.0.1:$port$path" >"$log" 2>&1; then
         results[$style,$proto]=$(parse "$log")
         echo "$(cut -d' ' -f1 <<<"${results[$style,$proto]}") req/s" >&2
      else
         results[$style,$proto]="- - - - - -"
         echo "h2load failed, see $log" >&2
      fi
   done
   stop_server
done

echo
echo "h2load -D ${duration}s -c $clients -m $streams (h1: -m 1), https://127.0.0.1:$port$path"
echo
row='%-8s %-5s %12s %10s %10s %7s %9s %9s\n'
printf "$row" build proto req/s MB/s succeeded failed median p99
printf "$row" -------- ----- ------------ ---------- ---------- ------- --------- ---------
for proto in "${protocols[@]}"; do
   for style in "${styles[@]}"; do
      read -r rps mbs ok failed median p99 <<<"${results[$style,$proto]}"
      printf "$row" "$style" "$proto" "$rps" "$mbs" "$ok" "$failed" "$median" "$p99"
   done
done

# corosio relative to asio, per protocol
echo
for proto in "${protocols[@]}"; do
   a=$(cut -d' ' -f1 <<<"${results[asio,$proto]}")
   c=$(cut -d' ' -f1 <<<"${results[corosio,$proto]}")
   if [[ $a != - && $c != - ]]; then
      awk -v p="$proto" -v a="$a" -v c="$c" \
         'BEGIN { printf "%s: corosio/asio = %.2f\n", p, (a > 0 ? c / a : 0) }'
   fi
done
echo
echo "logs: $out"
