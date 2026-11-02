// Vita overlay: EFB copies always land in an RGBA8 render texture; the GL replay converts to the
// requested GX copy format (intensity, alpha, depth, ...) while resolving, so callers only need
// to know whether a conversion step is involved.
#pragma once

#include "types.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {
inline bool needs_conversion(GXTexFmt fmt) {
  return fmt != GX_TF_RGBA8 && fmt != GX_TF_RGB565 && fmt != GX_TF_RGB5A3;
}
} // namespace aurora::gfx::tex_copy_conv
