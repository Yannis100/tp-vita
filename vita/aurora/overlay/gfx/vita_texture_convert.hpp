// CPU decoders from GX texture formats to RGBA8 for the Vita backend.
//
// Derived from the portable decoders of upstream aurora/lib/gfx/texture_convert.cpp (MIT): the same
// formulas and block layouts, rewritten for the Cortex-A9 (table lookups instead of per-texel bit
// math, one decode function per tile, direct stores into the destination for whole tiles) and
// decoupled from WebGPU so the code also builds and is unit-tested on the host.
//
// Pixels are packed as 0xAABBGGRR (little-endian host): memory order R, G, B, A.
#pragma once

#include <dolphin/gx/GXEnum.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::gfx {

// Number of RGBA8 texels in `mips` levels of a w x h texture (level n is max(w>>n,1) x max(h>>n,1)).
size_t rgba8_mip_texels(uint32_t width, uint32_t height, uint32_t mips) noexcept;
// Levels of a complete mip chain down to 1x1.
uint32_t full_mip_count(uint32_t width, uint32_t height) noexcept;

// Size in bytes of the GX-format source data (tiled, padded to whole tiles per level); 0 when the
// format is unsupported or a dimension is zero. Supports I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8
// C14X2 CMPR and the Aurora linear PC formats (R8_PC RG8_PC RGBA8_PC BC1_PC).
size_t gx_texture_source_size(uint32_t format, uint32_t width, uint32_t height, uint32_t mips) noexcept;
bool is_supported_texture_format(uint32_t format) noexcept;

// Decodes `mips` levels into `out` (resized to rgba8_mip_texels). Returns false when the format is
// unsupported (palette formats need decode_palette_texture) or `srcSize` is too small.
// When `arbitraryMips` is non-null and mips > 1 it receives upstream's "hand-authored mip chain"
// heuristic (only useful for anisotropic filtering, which the Vita backend does not use).
bool decode_texture(uint32_t format, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src,
                    size_t srcSize, std::vector<uint32_t>& out, bool* arbitraryMips = nullptr);

// Palette entries of a TLUT as RGBA8; `out` holds at least `entries` values.
bool decode_tlut(GXTlutFmt format, uint32_t entries, const uint8_t* src, size_t srcSize, std::vector<uint32_t>& out);

// C4 / C8 / C14X2 indices + TLUT -> RGBA8 (index >= entries gives transparent black, like upstream).
bool decode_palette_texture(uint32_t format, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src,
                            size_t srcSize, GXTlutFmt tlutFormat, uint32_t tlutEntries, const uint8_t* tlut,
                            size_t tlutSize, std::vector<uint32_t>& out, bool* arbitraryMips = nullptr);

// `pixels` holds `haveLevels` concatenated levels of a w x h texture; appends box-filtered levels
// (2x2 average, rounded) until it holds `targetLevels`. GL needs a complete chain before a mip
// filter can be used, and GX allows chains that stop early (max LOD).
void extend_mip_chain(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t haveLevels,
                      uint32_t targetLevels);

// Upstream's heuristic for manually authored mip chains (distance effects): average RMS difference
// between each level and the box-filtered level above it.
bool detect_arbitrary_mips(const uint32_t* pixels, uint32_t width, uint32_t height, uint32_t mips);

} // namespace aurora::gfx
