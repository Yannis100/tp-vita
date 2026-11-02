#!/bin/sh
# Build and run the shader-generator compile test on the host (Mesa GLES2):
#   tests/host/shader/run.sh [configs] [seed]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
aurora=${AURORA_DIR:-$root/../dusklight/extern/aurora}
python3 "$root/tools/build_aurora_tree.py" "$aurora/lib" \
  "$root/vita/aurora/overlay" "$root/vita/aurora/patches.py" "$root/build/aurora_lib" >/dev/null
out=${TPV_TEST_OUT:-/tmp/tpv/hostgl/test_shader}
CXXFLAGS="${TPV_CXXFLAGS:--O1 -w}" LDFLAGS="${TPV_LDFLAGS:-}" "$here/../build.sh" "$out" "$here/test_compile.cpp" \
  "$root/build/aurora_lib/gx/vita_shader.cpp" "$root/build/aurora_lib/gx/vita_vertex.cpp" \
  "$root/build/aurora_lib/gx/shader_info.cpp" "$root/build/aurora_lib/gx/attr_fmt.cpp" "$here/host_stubs.cpp"
exec "$out" "$@"
