#!/bin/sh
# Build and run the end-to-end host render harness:  tests/host/e2e/run.sh [scene ...]
# Needs: Mesa EGL/GLES2 (libegl1 libgles2), the VitaSDK (GL headers), a dusklight checkout (AURORA_DIR) and the
# fetched deps (tools/fetch_deps.sh). The merged Aurora tree is (re)assembled first. TPV_E2E_ASAN=1: AddressSanitizer build.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
aurora=${AURORA_DIR:-$root/../dusklight/extern/aurora}
inc=${HOST_GL_INC:-/tmp/tpv/hostgl/inc}
# An empty VITASDK would point the header links below at /arm-vita-eabi/... and break every later build.
if [ ! -d "${VITASDK:-}/arm-vita-eabi/include/GLES2" ]; then
  echo "run.sh: VITASDK must point at the VitaSDK (no \$VITASDK/arm-vita-eabi/include/GLES2 found)" >&2
  exit 2
fi
# TPV_E2E_ASAN=1 builds and runs the whole harness under AddressSanitizer (separate object directory). It found a real
# out-of-bounds write in the replay that the plain build only showed as a "double free" at exit.
if [ "${TPV_E2E_ASAN:-0}" = 1 ]; then
  asan_dir=${TPV_E2E_ASAN_DIR:-/tmp/tpv/e2e_asan}
  export CXXFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address"
  export OBJ="$asan_dir/obj" OUT="$asan_dir/e2e" TPV_E2E_OUT="$asan_dir/e2e"
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
fi
mkdir -p "$inc"
for d in GLES2 KHR EGL fmt; do ln -sfn "$VITASDK/arm-vita-eabi/include/$d" "$inc/$d"; done
python3 "$root/tools/build_aurora_tree.py" "$aurora/lib" "$root/vita/aurora/overlay" \
  "$root/vita/aurora/patches.py" "$root/build/aurora_lib" >/dev/null
make -s -f "$here/Makefile" -j"$(nproc)" AURORA_DIR="$aurora" GLINC="$inc" >/dev/null
exec "${TPV_E2E_OUT:-/tmp/tpv/e2e/e2e}" "$@"
