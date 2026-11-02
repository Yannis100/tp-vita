#!/bin/sh
# Host test for the crash symbol resolver. Builds the real crash_symbols.cpp against the real
# generated table from build/generated/crash_map.inc, so a broken parser or a stale table fails here
# rather than on the fatal path of a Vita.
#
# Usage: tests/host/crash/run.sh [build-dir]   (default: <repo>/build)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=${1:-$root/build}
inc=$build/generated/crash_map.inc

if [ ! -f "$inc" ]; then
  echo "no crash table at $inc - build tpv_vita once first (it is generated from the link map)" >&2
  exit 2
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# The table is ~1.8 MB of string literals, so a plain compile is fine on the host.
${CXX:-g++} -std=c++17 -O1 -g \
  -I"$build/generated" \
  "$root/vita/src/crash_symbols.cpp" \
  "$here/test_crash.cpp" \
  -o "$out/test_crash"
"$out/test_crash"
