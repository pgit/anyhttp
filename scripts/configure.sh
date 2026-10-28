#!/usr/bin/env bash
#
# Configures the out-of-source build trees described in CLAUDE.md: all of them, or the ones named.
# Each tree is a configure preset of the same name in CMakePresets.json, which is where the options
# of each one live -- this script only does several at once. Configuring an existing tree again is
# harmless; to change its compiler, delete it first.
#
# Usage: scripts/configure.sh [-b] [tree...]
#
#   -b  build the trees too
#
# Run from anywhere; the trees are created in the repo root. 'cmake --list-presets' names them all
# with what each one is for, and 'cmake --preset <tree>' configures a single one without this
# script.
#
set -euo pipefail

build=0
self=$(realpath "$0")

# the usage is this file's header comment, followed by the trees themselves
usage() {
   sed -n '/^set /q; 3,$p' "$self" | sed 's/^# \{0,1\}//'
   cmake --list-presets
}

cd "$(dirname "$self")/.."

while getopts "bh" opt; do
   case $opt in
   b) build=1 ;;
   *)
      usage
      exit 2
      ;;
   esac
done
shift $((OPTIND - 1))

# the trees are the non-hidden configure presets, in the order CMakePresets.json lists them
mapfile -t all < <(cmake --list-presets | sed -n 's/^  "\([^"]*\)".*/\1/p')
((${#all[@]})) || {
   echo "error: no configure presets in CMakePresets.json" >&2
   exit 1
}

(($#)) || set -- "${all[@]}"
trees=("${@%/}")

# check every name before configuring anything, so that a typo in the last one costs nothing
for tree in "${trees[@]}"; do
   printf '%s\n' "${all[@]}" | grep -qxF -- "$tree" || {
      echo "error: unknown tree '$tree', one of: ${all[*]}" >&2
      exit 2
   }
done

for tree in "${trees[@]}"; do
   echo "=== $tree"
   cmake --preset "$tree"
done

if ((build)); then
   for tree in "${trees[@]}"; do
      echo "=== building $tree"
      cmake --build --preset "$tree"
   done
fi
