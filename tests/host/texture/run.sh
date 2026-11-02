#!/bin/sh
# Differential test of the Vita texture decoders against upstream Aurora's converter (host, ASan/UBSan):
#   tests/host/texture/run.sh [rounds]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
aurora=${AURORA_DIR:-$root/../dusklight/extern/aurora}
inc=${HOST_GL_INC:-/tmp/tpv/hostgl/inc}
mkdir -p "$inc"
for d in KHR fmt; do ln -sfn "$VITASDK/arm-vita-eabi/include/$d" "$inc/$d"; done
# Upstream converter, unmodified, with the WebGPU-bound headers swapped for tiny shims.
shim=/tmp/tpv/difftex/lib
rm -rf /tmp/tpv/difftex
mkdir -p "$shim/gfx" "$shim/gx" "$shim/webgpu"
cp "$aurora/lib/gfx/texture_convert.cpp" "$aurora/lib/gfx/texture_convert.hpp" "$shim/gfx/"
cp "$aurora/lib/internal.hpp" "$aurora/lib/logging.hpp" "$shim/"
cp "$aurora/lib/gx/gx_fmt.hpp" "$shim/gx/"
cat > "$shim/webgpu/gpu.hpp" <<'SHIM'
#pragma once
namespace wgpu { enum class TextureFormat { RGBA8Unorm, R8Unorm, RG8Unorm, R16Sint, BC1RGBAUnorm }; }
namespace aurora::webgpu { inline bool g_textureComponentSwizzleSupported = false; inline bool g_bcTexturesSupported = false; }
SHIM
printf '#pragma once\n#include <dolphin/gx.h>\n#include "../internal.hpp"\n' > "$shim/gfx/texture.hpp"
printf '#pragma once\n#include "../internal.hpp"\n' > "$shim/gfx/types.hpp"
cat > "$shim/stubs.cpp" <<'SHIM'
#include "logging.hpp"
namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char* m, const char* s, unsigned int) noexcept { std::fprintf(stderr, "%s: %s\n", m, s); }
}
SHIM
sed -i '1i #include <cstdio>' "$shim/stubs.cpp"
out=${TPV_TEST_OUT:-/tmp/tpv/hostgl/test_texture}
g++ -std=gnu++20 -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -DFMT_HEADER_ONLY=1 -DAURORA -DTARGET_PC -I"$inc" -I"$aurora/include" -I"$root/deps/tracy/public" \
  -I"$shim" -I"$root/vita/aurora/overlay" "$here/test_vs_upstream.cpp" "$shim/gfx/texture_convert.cpp" "$shim/stubs.cpp" \
  "$root/vita/aurora/overlay/gfx/vita_texture_convert.cpp" -o "$out"
exec "$out" "$@"
