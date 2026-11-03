#!/bin/sh
# Build one of the two deliverables.
#
#   tools/build.sh release   optimised, asserts off   -> build/tpv_vita-release.vpk   (what ships)
#   tools/build.sh dev       -O1, asserts on          -> build/tpv_vita-dev.vpk       (reproduce bugs)
#   tools/build.sh debug     -O0, asserts on          -> build/tpv_vita-debug.vpk
#
# Why separate configuration directories: a build tree holds one set of flags, so sharing `build/`
# between variants means a full rebuild every time you switch, and a half-configured tree that is
# neither. Release keeps `build/` because every existing script, test and note refers to it; the dev
# variants get their own directories.
#
# Why `dev` is the default for reproducing: it keeps -O1 and frame pointers, so it is close to the
# shipped code and still debuggable. -O0 builds are often too slow to reach the frame where an
# intermittent bug lives, which makes them useless for exactly the bugs they are requested for.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
variant=${1:-dev}
shift 2>/dev/null || true

case "$variant" in
  release) dir=build;          type=Release ;;
  dev)     dir=build-dev;      type=Debug ;;
  debug)   dir=build-debug;    type=Debug ;;
  *) echo "usage: tools/build.sh [release|dev|debug] [extra cmake args]" >&2; exit 2 ;;
esac

# The variant is derived from CMAKE_BUILD_TYPE for release, and from a custom type for dev/debug so
# the two cannot be confused. See the top of CMakeLists.txt.
if [ "$variant" = release ]; then
  buildtype=Release
elif [ "$variant" = dev ]; then
  buildtype=Dev
else
  buildtype=Debug
fi

jobs=${TPV_JOBS:-$(nproc 2>/dev/null || echo 4)}
echo "==> $variant: $dir (CMAKE_BUILD_TYPE=$buildtype, -j$jobs)"
cmake -S "$root" -B "$root/$dir" -DCMAKE_BUILD_TYPE="$buildtype" -DTPV_PACKAGE=ON "$@"
cmake --build "$root/$dir" --target "tpv_vita-${variant}.vpk-vpk" -j "$jobs"

vpk="$root/$dir/tpv_vita-${variant}.vpk"
echo
echo "==> $vpk"
ls -l "$vpk"
