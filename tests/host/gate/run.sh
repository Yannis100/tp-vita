#!/bin/sh
# Build and run the filesystem-switch gate tests:  tests/host/gate/run.sh
#
# No GL, no overlay, no aurora tree: this suite tests decision logic only, so it builds against the
# host compiler in a second and never fails for a reason unrelated to the thing it guards. That is
# deliberate -- a suite that needs the renderer to link is a suite that silently stops being run
# (see the note in tests/host/vertex/run.sh about logging.cpp).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TPV_TEST_OUT:-/tmp/tpv/hostgate/test_gate}
mkdir -p "$(dirname "$out")"
CXX=${CXX:-g++}
$CXX -std=c++20 -O1 -g -Wall -Wextra -o "$out" "$here/gate_main.cpp"
exec "$out"
