#!/bin/sh
# Build a host (x86) test program that can use a GLES2 context and the merged Aurora tree:
#   tests/host/build.sh <out> <sources...>
# Extra flags can be passed through CXXFLAGS / LDFLAGS. GL/KHR/EGL/fmt headers come from the
# VitaSDK, symlinked into a scratch dir so the SDK's newlib headers do not shadow the host libc.
# Run tools/check_overlay.sh (or build_aurora_tree.py) first so build/aurora_lib is current.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
aurora=${AURORA_DIR:-$root/../dusklight/extern/aurora}
inc=${HOST_GL_INC:-/tmp/tpv/hostgl/inc}
mkdir -p "$inc"
for d in GLES2 KHR EGL fmt; do ln -sfn "$VITASDK/arm-vita-eabi/include/$d" "$inc/$d"; done
out=$1; shift
# -DEGL_NO_X11: EGL/eglplatform.h would otherwise include <X11/Xlib.h> (libx11-dev); a headless EGL test does not need it.
exec g++ -std=gnu++20 -O1 -g -DFMT_HEADER_ONLY=1 -DAURORA -DAURORA_ENABLE_GX -DTARGET_PC -DTARGET_VITA=1 -DEGL_NO_X11 \
  -I"$inc" -I"$here" -I"$aurora/include" -I"$root/build/aurora_lib" -I"$root/deps/xxhash" \
  -I"$root/deps/tracy/public" -I"$root/deps/abseil" $CXXFLAGS "$here/glctx.cpp" "$@" -o "$out" \
  /usr/lib/x86_64-linux-gnu/libEGL.so.1 /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 -lz $LDFLAGS
