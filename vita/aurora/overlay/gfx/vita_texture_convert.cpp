// CPU decoders from GX texture formats to RGBA8; see vita_texture_convert.hpp.
//
// Derived from upstream aurora/lib/gfx/texture_convert.cpp (MIT, encounter/aurora).
#include "vita_texture_convert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "pixels are packed as little-endian 0xAABBGGRR");

namespace aurora::gfx {
namespace {

// ---- pixel packing and bit expansion -----------------------------------------------------------
// http://www.mindcontrol.org/~hplus/graphics/expand-bits.html (same expansions as upstream)
constexpr uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) noexcept {
  return r | (g << 8) | (b << 16) | (a << 24);
}
constexpr uint32_t ex3(uint32_t n) noexcept { return (n << 5) | (n << 2) | (n >> 1); }
constexpr uint32_t ex4(uint32_t n) noexcept { return (n << 4) | n; }
constexpr uint32_t ex5(uint32_t n) noexcept { return (n << 3) | (n >> 2); }
constexpr uint32_t ex6(uint32_t n) noexcept { return (n << 2) | (n >> 4); }

// A big-endian 16-bit texel (b0 = high byte) is split into two byte lookups whose results are OR-ed
// together: every output channel bit comes from exactly one byte, so the expansions stay exact.
//
//   RGB565   b0 = RRRRRGGG   b1 = GGGBBBBB
//   RGB5A3   b0 = 1RRRRRGG   b1 = GGGBBBBB            (opaque, 5 bits per channel)
//            b0 = 0AAARRRR   b1 = GGGGBBBB            (3-bit alpha, 4 bits per channel)
constexpr std::array<uint32_t, 256> make_565_hi() noexcept {
  std::array<uint32_t, 256> t{};
  for (uint32_t b0 = 0; b0 < 256; ++b0) {
    const uint32_t gh = b0 & 7;
    t[b0] = pack(ex5(b0 >> 3), (gh << 5) | (gh >> 1), 0, 0xff);
  }
  return t;
}
constexpr std::array<uint32_t, 256> make_565_lo() noexcept {
  std::array<uint32_t, 256> t{};
  for (uint32_t b1 = 0; b1 < 256; ++b1) {
    t[b1] = pack(0, (b1 >> 5) << 2, ex5(b1 & 0x1f), 0);
  }
  return t;
}
constexpr std::array<uint32_t, 256> make_5a3_hi() noexcept {
  std::array<uint32_t, 256> t{};
  for (uint32_t b0 = 0; b0 < 256; ++b0) {
    if (b0 & 0x80) {
      const uint32_t gh = b0 & 3;
      t[b0] = pack(ex5((b0 >> 2) & 0x1f), (gh << 6) | (gh << 1), 0, 0xff);
    } else {
      t[b0] = pack(ex4(b0 & 0xf), 0, 0, ex3((b0 >> 4) & 7));
    }
  }
  return t;
}
constexpr std::array<std::array<uint32_t, 256>, 2> make_5a3_lo() noexcept {
  std::array<std::array<uint32_t, 256>, 2> t{};
  for (uint32_t b1 = 0; b1 < 256; ++b1) {
    t[0][b1] = pack(0, ex4(b1 >> 4), ex4(b1 & 0xf), 0);
    const uint32_t gl = b1 >> 5;
    t[1][b1] = pack(0, (gl << 3) | (gl >> 2), ex5(b1 & 0x1f), 0);
  }
  return t;
}
constexpr std::array<uint32_t, 16> make_i4() noexcept {
  std::array<uint32_t, 16> t{};
  for (uint32_t n = 0; n < 16; ++n) {
    t[n] = pack(ex4(n), ex4(n), ex4(n), ex4(n));
  }
  return t;
}
constexpr std::array<uint32_t, 256> make_ia4() noexcept {
  std::array<uint32_t, 256> t{};
  for (uint32_t b = 0; b < 256; ++b) {
    t[b] = pack(ex4(b & 0xf), ex4(b & 0xf), ex4(b & 0xf), ex4(b >> 4));
  }
  return t;
}

constexpr auto k565Hi = make_565_hi();
constexpr auto k565Lo = make_565_lo();
constexpr auto k5a3Hi = make_5a3_hi();
constexpr auto k5a3Lo = make_5a3_lo();
constexpr auto kI4 = make_i4();
constexpr auto kIA4 = make_ia4();

inline uint32_t rgb565(const uint8_t* s) noexcept { return k565Hi[s[0]] | k565Lo[s[1]]; }
inline uint32_t rgb5a3(const uint8_t* s) noexcept { return k5a3Hi[s[0]] | k5a3Lo[s[0] >> 7][s[1]]; }
// IA8: byte 0 = alpha, byte 1 = intensity.
inline uint32_t ia8(const uint8_t* s) noexcept { return pack(s[1], s[1], s[1], s[0]); }

// ---- tile decoders ----------------------------------------------------------------------------
// Each decodes one whole tile of Bytes bytes into BW x BH pixels at d (row stride `stride` pixels).

struct DecI4 {
  static constexpr uint32_t BW = 8, BH = 8, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 8; ++y, s += 4, d += stride) {
      for (uint32_t k = 0; k < 4; ++k) {
        d[2 * k] = kI4[s[k] >> 4];
        d[2 * k + 1] = kI4[s[k] & 0xf];
      }
    }
  }
};
struct DecI8 {
  static constexpr uint32_t BW = 8, BH = 4, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 8; ++x) {
        d[x] = s[x] * 0x01010101u;
      }
    }
  }
};
struct DecIA4 {
  static constexpr uint32_t BW = 8, BH = 4, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 8; ++x) {
        d[x] = kIA4[s[x]];
      }
    }
  }
};
struct DecIA8 {
  static constexpr uint32_t BW = 4, BH = 4, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 4; ++x) {
        d[x] = ia8(s + 2 * x);
      }
    }
  }
};
struct DecRGB565 {
  static constexpr uint32_t BW = 4, BH = 4, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 4; ++x) {
        d[x] = rgb565(s + 2 * x);
      }
    }
  }
};
struct DecRGB5A3 {
  static constexpr uint32_t BW = 4, BH = 4, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 4; ++x) {
        d[x] = rgb5a3(s + 2 * x);
      }
    }
  }
};
// RGBA8: two 32-byte planes per tile, first (A,R) pairs then (G,B) pairs, row-major.
struct DecRGBA8 {
  static constexpr uint32_t BW = 4, BH = 4, Bytes = 64;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    const uint8_t* gb = s + 32;
    for (uint32_t y = 0; y < 4; ++y, s += 8, gb += 8, d += stride) {
      for (uint32_t x = 0; x < 4; ++x) {
        d[x] = pack(s[2 * x + 1], gb[2 * x], gb[2 * x + 1], s[2 * x]);
      }
    }
  }
};

// Per-channel (3a + 5b) / 8, GX's S3TC interpolation weights; alpha is opaque.
constexpr uint32_t blend35(uint32_t a, uint32_t b) noexcept {
  uint32_t r = 0xff000000u;
  for (uint32_t shift = 0; shift < 24; shift += 8) {
    r |= (((3 * ((a >> shift) & 0xff) + 5 * ((b >> shift) & 0xff)) >> 3) & 0xff) << shift;
  }
  return r;
}
constexpr uint32_t half_blend(uint32_t a, uint32_t b) noexcept {
  uint32_t r = 0xff000000u;
  for (uint32_t shift = 0; shift < 24; shift += 8) {
    r |= ((((a >> shift) & 0xff) + ((b >> shift) & 0xff)) >> 1) << shift;
  }
  return r;
}

// One 4x4 CMPR sub-block: big-endian endpoints, then 4 index bytes (one per row, 2 bits per texel,
// leftmost texel in the top bits). When color1 <= color2 index 3 is a transparent midpoint.
inline void cmpr_subblock(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
  const uint32_t c0 = (uint32_t(s[0]) << 8) | s[1];
  const uint32_t c1 = (uint32_t(s[2]) << 8) | s[3];
  const uint32_t p0 = rgb565(s);
  const uint32_t p1 = rgb565(s + 2);
  uint32_t table[4];
  table[0] = p0;
  table[1] = p1;
  if (c0 > c1) {
    table[2] = blend35(p1, p0);
    table[3] = blend35(p0, p1);
  } else {
    table[2] = half_blend(p0, p1);
    table[3] = table[2] & 0x00ffffffu; // CMPR: alpha-0 midpoint, not black
  }
  for (uint32_t y = 0; y < 4; ++y, d += stride) {
    const uint32_t bits = s[4 + y];
    d[0] = table[(bits >> 6) & 3];
    d[1] = table[(bits >> 4) & 3];
    d[2] = table[(bits >> 2) & 3];
    d[3] = table[bits & 3];
  }
}
struct DecCMPR {
  static constexpr uint32_t BW = 8, BH = 8, Bytes = 32;
  static void decode(const uint8_t* s, uint32_t* d, size_t stride) noexcept {
    cmpr_subblock(s, d, stride);
    cmpr_subblock(s + 8, d + 4, stride);
    cmpr_subblock(s + 16, d + 4 * stride, stride);
    cmpr_subblock(s + 24, d + 4 * stride + 4, stride);
  }
};

// Palette-indexed formats map each index through a padded RGBA8 palette.
struct DecC4 {
  static constexpr uint32_t BW = 8, BH = 8, Bytes = 32;
  const uint32_t* pal; // >= 16 entries
  void decode(const uint8_t* s, uint32_t* d, size_t stride) const noexcept {
    for (uint32_t y = 0; y < 8; ++y, s += 4, d += stride) {
      for (uint32_t k = 0; k < 4; ++k) {
        d[2 * k] = pal[s[k] >> 4];
        d[2 * k + 1] = pal[s[k] & 0xf];
      }
    }
  }
};
struct DecC8 {
  static constexpr uint32_t BW = 8, BH = 4, Bytes = 32;
  const uint32_t* pal; // >= 256 entries
  void decode(const uint8_t* s, uint32_t* d, size_t stride) const noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 8; ++x) {
        d[x] = pal[s[x]];
      }
    }
  }
};
struct DecC14X2 {
  static constexpr uint32_t BW = 4, BH = 4, Bytes = 32;
  const uint32_t* pal; // >= 16384 entries
  void decode(const uint8_t* s, uint32_t* d, size_t stride) const noexcept {
    for (uint32_t y = 0; y < 4; ++y, s += 8, d += stride) {
      for (uint32_t x = 0; x < 4; ++x) {
        d[x] = pal[((uint32_t(s[2 * x]) << 8) | s[2 * x + 1]) & 0x3fff];
      }
    }
  }
};

// Stateless decoders adapt to the same call shape as the palette ones.
template <class D>
struct Stateless {
  static constexpr uint32_t BW = D::BW, BH = D::BH, Bytes = D::Bytes;
  void decode(const uint8_t* s, uint32_t* d, size_t stride) const noexcept { D::decode(s, d, stride); }
};

// Decodes `mips` tiled levels. Whole tiles are stored straight into the destination; tiles that
// overhang the right/bottom edge of a level go through a scratch tile first.
template <class Dec>
bool decode_tiled(const Dec& dec, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src,
                  size_t srcSize, uint32_t* out) noexcept {
  constexpr uint32_t BW = Dec::BW;
  constexpr uint32_t BH = Dec::BH;
  const uint8_t* const end = src + srcSize;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t lw = std::max(width >> mip, 1u);
    const uint32_t lh = std::max(height >> mip, 1u);
    const uint32_t tilesX = (lw + BW - 1) / BW;
    const uint32_t tilesY = (lh + BH - 1) / BH;
    if (static_cast<size_t>(end - src) < static_cast<size_t>(tilesX) * tilesY * Dec::Bytes) {
      return false;
    }
    for (uint32_t ty = 0; ty < tilesY; ++ty) {
      const uint32_t y0 = ty * BH;
      for (uint32_t tx = 0; tx < tilesX; ++tx, src += Dec::Bytes) {
        const uint32_t x0 = tx * BW;
        uint32_t* dst = out + static_cast<size_t>(y0) * lw + x0;
        if (x0 + BW <= lw && y0 + BH <= lh) {
          dec.decode(src, dst, lw);
        } else {
          uint32_t tile[BW * BH];
          dec.decode(src, tile, BW);
          const uint32_t rows = std::min(BH, lh - y0);
          const uint32_t cols = std::min(BW, lw - x0);
          for (uint32_t r = 0; r < rows; ++r) {
            std::memcpy(dst + static_cast<size_t>(r) * lw, tile + r * BW, cols * sizeof(uint32_t));
          }
        }
      }
    }
    out += static_cast<size_t>(lw) * lh;
  }
  return true;
}

// ---- Aurora linear PC formats ------------------------------------------------------------------
bool decode_r8(uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src, size_t srcSize,
               uint32_t* out) noexcept {
  if (srcSize < gx_texture_source_size(GX_TF_R8_PC, width, height, mips)) {
    return false;
  }
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const size_t n = static_cast<size_t>(std::max(width >> mip, 1u)) * std::max(height >> mip, 1u);
    for (size_t i = 0; i < n; ++i) {
      out[i] = src[i] * 0x01010101u;
    }
    out += n;
    src += n;
  }
  return true;
}
bool decode_rg8(uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src, size_t srcSize,
                uint32_t* out) noexcept {
  if (srcSize < gx_texture_source_size(GX_TF_RG8_PC, width, height, mips)) {
    return false;
  }
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const size_t n = static_cast<size_t>(std::max(width >> mip, 1u)) * std::max(height >> mip, 1u);
    for (size_t i = 0; i < n; ++i) {
      out[i] = pack(src[2 * i], src[2 * i], src[2 * i], src[2 * i + 1]);
    }
    out += n;
    src += n * 2;
  }
  return true;
}
bool decode_rgba8_pc(uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src, size_t srcSize,
                     uint32_t* out) noexcept {
  const size_t bytes = rgba8_mip_texels(width, height, mips) * 4;
  if (srcSize < bytes) {
    return false;
  }
  std::memcpy(out, src, bytes);
  return true;
}
// BC1 as stored by PC tools: little-endian endpoints, 32 index bits (LSB first, row-major).
bool decode_bc1(uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src, size_t srcSize,
                uint32_t* out) noexcept {
  if (srcSize < gx_texture_source_size(GX_TF_BC1_PC, width, height, mips)) {
    return false;
  }
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t lw = std::max(width >> mip, 1u);
    const uint32_t lh = std::max(height >> mip, 1u);
    for (uint32_t yy = 0; yy < lh; yy += 4) {
      for (uint32_t xx = 0; xx < lw; xx += 4, src += 8) {
        const uint32_t c0 = src[0] | (uint32_t(src[1]) << 8);
        const uint32_t c1 = src[2] | (uint32_t(src[3]) << 8);
        const uint32_t indices = src[4] | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) | (uint32_t(src[7]) << 24);
        const uint8_t b0[2] = {static_cast<uint8_t>(c0 >> 8), static_cast<uint8_t>(c0)};
        const uint8_t b1[2] = {static_cast<uint8_t>(c1 >> 8), static_cast<uint8_t>(c1)};
        const uint32_t p0 = rgb565(b0);
        const uint32_t p1 = rgb565(b1);
        uint32_t table[4] = {p0, p1, 0, 0};
        if (c0 > c1) {
          table[2] = blend35(p1, p0);
          table[3] = blend35(p0, p1);
        } else {
          table[2] = half_blend(p0, p1);
          table[3] = 0;
        }
        for (uint32_t y = 0; y < 4 && yy + y < lh; ++y) {
          for (uint32_t x = 0; x < 4 && xx + x < lw; ++x) {
            out[static_cast<size_t>(yy + y) * lw + xx + x] = table[(indices >> (2 * (y * 4 + x))) & 3];
          }
        }
      }
    }
    out += static_cast<size_t>(lw) * lh;
  }
  return true;
}

constexpr uint32_t div_ceil(uint32_t value, uint32_t divisor) noexcept { return (value + divisor - 1) / divisor; }

// Box downscale (2x2, rounded) of one RGBA8 level; coordinates clamp at the edges like upstream.
void downscale(const uint32_t* src, uint32_t sw, uint32_t sh, uint32_t* dst, uint32_t dw, uint32_t dh) noexcept {
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = std::min(y * 2, sh - 1);
    const uint32_t y1 = std::min(y0 + 1, sh - 1);
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = std::min(x * 2, sw - 1);
      const uint32_t x1 = std::min(x0 + 1, sw - 1);
      const uint32_t a = src[static_cast<size_t>(y0) * sw + x0];
      const uint32_t b = src[static_cast<size_t>(y0) * sw + x1];
      const uint32_t c = src[static_cast<size_t>(y1) * sw + x0];
      const uint32_t e = src[static_cast<size_t>(y1) * sw + x1];
      uint32_t r = 0;
      for (uint32_t shift = 0; shift < 32; shift += 8) {
        const uint32_t sum = ((a >> shift) & 0xff) + ((b >> shift) & 0xff) + ((c >> shift) & 0xff) + ((e >> shift) & 0xff);
        r |= ((sum + 2) / 4) << shift;
      }
      dst[static_cast<size_t>(y) * dw + x] = r;
    }
  }
}

constexpr float kArbMipThreshold = 15.0f;

} // namespace

// ---- public API --------------------------------------------------------------------------------

size_t rgba8_mip_texels(uint32_t width, uint32_t height, uint32_t mips) noexcept {
  size_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    total += static_cast<size_t>(std::max(width >> mip, 1u)) * std::max(height >> mip, 1u);
  }
  return total;
}

uint32_t full_mip_count(uint32_t width, uint32_t height) noexcept {
  uint32_t n = 1;
  for (uint32_t v = std::max(width, height); v > 1; v >>= 1) {
    ++n;
  }
  return n;
}

bool is_supported_texture_format(uint32_t format) noexcept {
  switch (format) {
  case GX_TF_I4:
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_RGBA8:
  case GX_TF_CMPR:
  case GX_TF_C4:
  case GX_TF_C8:
  case GX_TF_C14X2:
  case GX_TF_R8_PC:
  case GX_TF_RG8_PC:
  case GX_TF_RGBA8_PC:
  case GX_TF_BC1_PC:
    return true;
  default:
    return false;
  }
}

size_t gx_texture_source_size(uint32_t format, uint32_t width, uint32_t height, uint32_t mips) noexcept {
  if (width == 0 || height == 0 || mips == 0) {
    return 0;
  }
  uint64_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t w = std::max(width >> mip, 1u);
    const uint32_t h = std::max(height >> mip, 1u);
    switch (format) {
    case GX_TF_R8_PC:
      total += static_cast<uint64_t>(w) * h;
      break;
    case GX_TF_RG8_PC:
      total += static_cast<uint64_t>(w) * h * 2;
      break;
    case GX_TF_RGBA8_PC:
      total += static_cast<uint64_t>(w) * h * 4;
      break;
    case GX_TF_BC1_PC:
      total += static_cast<uint64_t>(div_ceil(w, 4)) * div_ceil(h, 4) * 8;
      break;
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR:
      total += static_cast<uint64_t>(div_ceil(w, 8)) * div_ceil(h, 8) * 32;
      break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8:
      total += static_cast<uint64_t>(div_ceil(w, 8)) * div_ceil(h, 4) * 32;
      break;
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_C14X2:
      total += static_cast<uint64_t>(div_ceil(w, 4)) * div_ceil(h, 4) * 32;
      break;
    case GX_TF_RGBA8:
      total += static_cast<uint64_t>(div_ceil(w, 4)) * div_ceil(h, 4) * 64;
      break;
    default:
      return 0;
    }
  }
  return total <= SIZE_MAX ? static_cast<size_t>(total) : 0;
}

bool decode_texture(uint32_t format, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src,
                    size_t srcSize, std::vector<uint32_t>& out, bool* arbitraryMips) {
  if (arbitraryMips != nullptr) {
    *arbitraryMips = false;
  }
  if (width == 0 || height == 0 || mips == 0 || src == nullptr) {
    return false;
  }
  out.resize(rgba8_mip_texels(width, height, mips));
  uint32_t* dst = out.data();
  bool ok = false;
  switch (format) {
  case GX_TF_I4:
    ok = decode_tiled(Stateless<DecI4>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_I8:
    ok = decode_tiled(Stateless<DecI8>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_IA4:
    ok = decode_tiled(Stateless<DecIA4>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_IA8:
    ok = decode_tiled(Stateless<DecIA8>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_RGB565:
    ok = decode_tiled(Stateless<DecRGB565>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_RGB5A3:
    ok = decode_tiled(Stateless<DecRGB5A3>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_RGBA8:
    ok = decode_tiled(Stateless<DecRGBA8>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_CMPR:
    ok = decode_tiled(Stateless<DecCMPR>{}, width, height, mips, src, srcSize, dst);
    break;
  case GX_TF_R8_PC:
    return decode_r8(width, height, mips, src, srcSize, dst);
  case GX_TF_RG8_PC:
    return decode_rg8(width, height, mips, src, srcSize, dst);
  case GX_TF_RGBA8_PC:
    return decode_rgba8_pc(width, height, mips, src, srcSize, dst);
  case GX_TF_BC1_PC:
    return decode_bc1(width, height, mips, src, srcSize, dst);
  default:
    break; // palette formats and unsupported formats
  }
  if (ok && arbitraryMips != nullptr && mips > 1) {
    *arbitraryMips = detect_arbitrary_mips(out.data(), width, height, mips);
  }
  return ok;
}

bool decode_tlut(GXTlutFmt format, uint32_t entries, const uint8_t* src, size_t srcSize,
                 std::vector<uint32_t>& out) {
  if (entries == 0 || src == nullptr || srcSize < static_cast<size_t>(entries) * 2) {
    return false;
  }
  out.resize(entries);
  switch (format) {
  case GX_TL_IA8:
    for (uint32_t i = 0; i < entries; ++i) {
      out[i] = ia8(src + 2 * i);
    }
    return true;
  case GX_TL_RGB565:
    for (uint32_t i = 0; i < entries; ++i) {
      out[i] = rgb565(src + 2 * i);
    }
    return true;
  case GX_TL_RGB5A3:
    for (uint32_t i = 0; i < entries; ++i) {
      out[i] = rgb5a3(src + 2 * i);
    }
    return true;
  default:
    return false;
  }
}

bool decode_palette_texture(uint32_t format, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* src,
                            size_t srcSize, GXTlutFmt tlutFormat, uint32_t tlutEntries, const uint8_t* tlut,
                            size_t tlutSize, std::vector<uint32_t>& out, bool* arbitraryMips) {
  if (arbitraryMips != nullptr) {
    *arbitraryMips = false;
  }
  if (width == 0 || height == 0 || mips == 0 || src == nullptr) {
    return false;
  }
  uint32_t padded = 0;
  switch (format) {
  case GX_TF_C4:
    padded = 16;
    break;
  case GX_TF_C8:
    padded = 256;
    break;
  case GX_TF_C14X2:
    padded = 16384;
    break;
  default:
    return false;
  }
  std::vector<uint32_t> palette;
  if (!decode_tlut(tlutFormat, tlutEntries, tlut, tlutSize, palette)) {
    return false;
  }
  // Indices at or past the end of the TLUT read as transparent black (upstream behaviour). A TLUT
  // longer than the index range is truncated.
  palette.resize(padded, 0);

  out.resize(rgba8_mip_texels(width, height, mips));
  bool ok = false;
  switch (format) {
  case GX_TF_C4:
    ok = decode_tiled(DecC4{palette.data()}, width, height, mips, src, srcSize, out.data());
    break;
  case GX_TF_C8:
    ok = decode_tiled(DecC8{palette.data()}, width, height, mips, src, srcSize, out.data());
    break;
  default:
    ok = decode_tiled(DecC14X2{palette.data()}, width, height, mips, src, srcSize, out.data());
    break;
  }
  if (ok && arbitraryMips != nullptr && mips > 1) {
    *arbitraryMips = detect_arbitrary_mips(out.data(), width, height, mips);
  }
  return ok;
}

void extend_mip_chain(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t haveLevels,
                      uint32_t targetLevels) {
  if (haveLevels == 0 || haveLevels >= targetLevels) {
    return;
  }
  size_t offset = rgba8_mip_texels(width, height, haveLevels - 1);
  uint32_t lw = std::max(width >> (haveLevels - 1), 1u);
  uint32_t lh = std::max(height >> (haveLevels - 1), 1u);
  pixels.resize(rgba8_mip_texels(width, height, targetLevels));
  for (uint32_t level = haveLevels; level < targetLevels; ++level) {
    const uint32_t nw = std::max(lw >> 1, 1u);
    const uint32_t nh = std::max(lh >> 1, 1u);
    const size_t next = offset + static_cast<size_t>(lw) * lh;
    downscale(pixels.data() + offset, lw, lh, pixels.data() + next, nw, nh);
    offset = next;
    lw = nw;
    lh = nh;
  }
}

bool detect_arbitrary_mips(const uint32_t* pixels, uint32_t width, uint32_t height, uint32_t mips) {
  if (mips < 2) {
    return false;
  }
  std::vector<uint32_t> scaled;
  std::vector<uint32_t> prev;
  const uint32_t* levelPtr = pixels;
  uint32_t lw = width;
  uint32_t lh = height;
  float totalDiff = 0.f;
  for (uint32_t mip = 0; mip + 1 < mips; ++mip) {
    const uint32_t nw = std::max(lw >> 1, 1u);
    const uint32_t nh = std::max(lh >> 1, 1u);
    const uint32_t* above = mip == 0 ? levelPtr : prev.data();
    scaled.resize(static_cast<size_t>(nw) * nh);
    downscale(above, lw, lh, scaled.data(), nw, nh);
    const uint32_t* actual = levelPtr + static_cast<size_t>(lw) * lh;
    double sum = 0.0;
    const size_t bytes = static_cast<size_t>(nw) * nh * 4;
    const auto* a = reinterpret_cast<const uint8_t*>(actual);
    const auto* b = reinterpret_cast<const uint8_t*>(scaled.data());
    for (size_t i = 0; i < bytes; ++i) {
      const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
      sum += static_cast<double>(diff * diff);
    }
    totalDiff += static_cast<float>(std::sqrt(sum / static_cast<double>(bytes)) / 2.56);
    // Upstream box-filters from the previous *downscaled* level, not from the stored one.
    prev.swap(scaled);
    levelPtr = actual;
    lw = nw;
    lh = nh;
  }
  return (totalDiff / static_cast<float>(mips - 1)) > kArbMipThreshold;
}

} // namespace aurora::gfx
