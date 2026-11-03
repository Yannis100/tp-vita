#!/bin/sh
# Host test for the crash symbol resolver. Builds the real crash_symbols.cpp and points it at the
# real table generated from the real ELF, so a broken generator or parser fails here rather than on
# the fatal path of a Vita.
#
# The table is a file now, not a compiled-in blob, so this test also proves the file format round
# trips: the generator writes it, the resolver reads it, and an address inside a C++ function with
# a spaces-and-commas name resolves to that function.
#
# Usage: tests/host/crash/run.sh [build-dir]   (default: <repo>/build)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=${1:-$root/build}
table=$build/crash_symbols.txt

if [ ! -f "$table" ]; then
  echo "no crash symbol table at $table" >&2
  echo "build tpv_vita once first: it is generated from the linked ELF (see CMakeLists.txt)" >&2
  exit 2
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

${CXX:-g++} -std=c++17 -O1 -g \
  "$root/vita/src/crash_symbols.cpp" \
  "$here/test_crash.cpp" \
  -o "$out/test_crash"
"$out/test_crash" "$table"
