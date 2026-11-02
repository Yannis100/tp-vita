// Vita overlay of aurora/lib/gfx/depth_peek.hpp: EFB depth readback (GXPeekZ) is not supported
// on Vita yet; read_latest() always reports "no data".
#pragma once

#include "types.hpp"

namespace aurora::gfx::depth_peek {
inline void initialize() {}
inline void shutdown() {}
inline void request_snapshot() noexcept {}
inline bool read_latest(uint16_t, uint16_t, uint32_t&) noexcept { return false; }
} // namespace aurora::gfx::depth_peek
