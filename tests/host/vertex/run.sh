#!/bin/sh
# Build and run the vertex decoder tests on the host:  tests/host/vertex/run.sh [bench]
# Needs the merged Aurora tree (tools/check_overlay.sh or tools/build_aurora_tree.py creates it).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
aurora=${AURORA_DIR:-$root/../dusklight/extern/aurora}
python3 "$root/tools/build_aurora_tree.py" "$aurora/lib" \
  "$root/vita/aurora/overlay" "$root/vita/aurora/patches.py" "$root/build/aurora_lib" >/dev/null
out=${TPV_TEST_OUT:-/tmp/tpv/hostgl/test_vertex}
if [ "$1" = bench ]; then
  CXXFLAGS="-O2 -Wall -Wno-unused-function" "$here/../build.sh" "$out" "$here/test_vertex.cpp" "$root/build/aurora_lib/gx/vita_vertex.cpp"
  exec "$out" bench
fi
CXXFLAGS="-O1 -Wall -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
  LDFLAGS="-fsanitize=address,undefined" \
  "$here/../build.sh" "$out" "$here/test_vertex.cpp" "$root/build/aurora_lib/gx/vita_vertex.cpp"
exec "$out"
