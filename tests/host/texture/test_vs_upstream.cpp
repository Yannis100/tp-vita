// Differential test: gfx/vita_texture_convert.cpp (Vita CPU decoders) against upstream Aurora's
// gfx/texture_convert.cpp compiled unmodified on the host (WebGPU replaced by a shim, see run.sh).
// For every GX colour/palette format, random data at awkward sizes and mip counts must decode to
// byte-identical RGBA8.
//
//   tests/host/texture/run.sh [rounds]
#include "gfx/vita_texture_convert.hpp"
#include "gfx/texture_convert.hpp" // upstream (shimmed)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace aurora;

namespace {
uint64_t g_s = 88172645463325252ull;
uint32_t rnd() {
  g_s ^= g_s << 13;
  g_s ^= g_s >> 7;
  g_s ^= g_s << 17;
  return static_cast<uint32_t>(g_s >> 11);
}
int g_checks = 0, g_fail = 0;

const char* fmt_name(uint32_t f) {
  switch (f) {
  case GX_TF_I4: return "I4";
  case GX_TF_I8: return "I8";
  case GX_TF_IA4: return "IA4";
  case GX_TF_IA8: return "IA8";
  case GX_TF_RGB565: return "RGB565";
  case GX_TF_RGB5A3: return "RGB5A3";
  case GX_TF_RGBA8: return "RGBA8";
  case GX_TF_CMPR: return "CMPR";
  case GX_TF_C4: return "C4";
  case GX_TF_C8: return "C8";
  case GX_TF_C14X2: return "C14X2";
  default: return "?";
  }
}

struct Size { uint32_t w, h; };
const Size kSizes[] = {{1, 1}, {2, 2}, {3, 5}, {4, 4}, {8, 8}, {10, 6}, {16, 16}, {32, 8}, {33, 17},
                       {64, 64}, {7, 30}, {128, 32}, {256, 256}, {5, 1}, {1, 9}};

bool compare(const char* what, uint32_t f, uint32_t w, uint32_t h, uint32_t mips, const std::vector<uint32_t>& ours,
             const gfx::ConvertedTexture& up) {
  ++g_checks;
  const size_t n = gfx::rgba8_mip_texels(w, h, mips);
  if (ours.size() != n || up.data.size() != n * 4 || std::memcmp(ours.data(), up.data.data(), n * 4) != 0) {
    ++g_fail;
    if (g_fail <= 15) {
      size_t first = 0;
      if (ours.size() == n && up.data.size() == n * 4) {
        while (first < n && std::memcmp(&ours[first], up.data.data() + first * 4, 4) == 0) ++first;
      }
      std::printf("MISMATCH %s fmt=%s %ux%u mips=%u ours=%zu texels upstream=%zu bytes (expected %zu texels) first diff texel %zu", what,
                  fmt_name(f), w, h, mips, ours.size(), up.data.size(), n, first);
      if (first < n && ours.size() == n && up.data.size() == n * 4) {
        uint32_t u;
        std::memcpy(&u, up.data.data() + first * 4, 4);
        std::printf(" ours=%08x upstream=%08x", ours[first], u);
      }
      std::printf("\n");
    }
    return false;
  }
  return true;
}
} // namespace

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 3;
  { // Negative control: a corrupted decode must be reported as a mismatch, or the comparison proves nothing.
    std::vector<uint8_t> src(gfx::gx_texture_source_size(GX_TF_RGB565, 8, 8, 1));
    for (auto& b : src) b = static_cast<uint8_t>(rnd());
    std::vector<uint32_t> ours;
    gfx::decode_texture(GX_TF_RGB565, 8, 8, 1, src.data(), src.size(), ours);
    ours[17] ^= 0x00010000u;
    const auto up = gfx::convert_texture(GX_TF_RGB565, 8, 8, 1, ArrayRef<uint8_t>(src.data(), src.size()));
    const int failsBefore = g_fail;
    const bool same = compare("negative-control", GX_TF_RGB565, 8, 8, 1, ours, up);
    if (same || g_fail != failsBefore + 1) {
      std::printf("negative control NOT detected - comparison is broken\n");
      return 3;
    }
    g_fail = failsBefore; // expected failure, do not count it
    g_checks -= 1;
    std::printf("negative control detected (expected mismatch above)\n");
  }
  const uint32_t colourFormats[] = {GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_RGBA8, GX_TF_CMPR};
  for (int round = 0; round < rounds; ++round) {
    for (const uint32_t f : colourFormats) {
      for (const Size sz : kSizes) {
        for (uint32_t mips = 1; mips <= 3; ++mips) {
          const size_t srcSize = gfx::gx_texture_source_size(f, sz.w, sz.h, mips);
          ++g_checks;
          if (srcSize == 0) {
            ++g_fail;
            std::printf("gx_texture_source_size==0 fmt=%s %ux%u mips=%u\n", fmt_name(f), sz.w, sz.h, mips);
            continue;
          }
          std::vector<uint8_t> src(srcSize);
          for (auto& b : src) b = static_cast<uint8_t>(rnd());
          std::vector<uint32_t> ours;
          if (!gfx::decode_texture(f, sz.w, sz.h, mips, src.data(), src.size(), ours)) {
            ++g_fail;
            std::printf("decode_texture failed fmt=%s %ux%u mips=%u\n", fmt_name(f), sz.w, sz.h, mips);
            continue;
          }
          const auto up = gfx::convert_texture(f, sz.w, sz.h, mips, ArrayRef<uint8_t>(src.data(), src.size()));
          compare("color", f, sz.w, sz.h, mips, ours, up);
        }
      }
    }
    // Palette formats: indices + TLUT -> RGBA8.
    const uint32_t palFormats[] = {GX_TF_C4, GX_TF_C8, GX_TF_C14X2};
    const GXTlutFmt tlutFormats[] = {GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3};
    for (const uint32_t f : palFormats) {
      for (const Size sz : kSizes) {
        for (uint32_t mips = 1; mips <= 2; ++mips) {
          for (const GXTlutFmt tf : tlutFormats) {
            const uint32_t maxEntries = f == GX_TF_C4 ? 16 : f == GX_TF_C8 ? 256 : 16384;
            // Include a short TLUT (fewer entries than the index range) to exercise the out-of-range rule.
            const uint32_t entries = (rnd() % 3 == 0) ? 1 + rnd() % maxEntries : maxEntries;
            const size_t srcSize = gfx::gx_texture_source_size(f, sz.w, sz.h, mips);
            std::vector<uint8_t> src(srcSize);
            for (auto& b : src) b = static_cast<uint8_t>(rnd());
            std::vector<uint8_t> tlut(static_cast<size_t>(entries) * 2);
            for (auto& b : tlut) b = static_cast<uint8_t>(rnd());
            std::vector<uint32_t> ours;
            const bool ok = gfx::decode_palette_texture(f, sz.w, sz.h, mips, src.data(), src.size(), tf, entries,
                                                        tlut.data(), tlut.size(), ours);
            ++g_checks;
            if (!ok) {
              ++g_fail;
              std::printf("decode_palette_texture failed fmt=%s %ux%u\n", fmt_name(f), sz.w, sz.h);
              continue;
            }
            const auto up = gfx::convert_texture_palette(f, sz.w, sz.h, mips, ArrayRef<uint8_t>(src.data(), src.size()), tf,
                                                         static_cast<uint16_t>(entries),
                                                         ArrayRef<uint8_t>(tlut.data(), tlut.size()));
            compare("palette", f, sz.w, sz.h, mips, ours, up);
          }
        }
      }
    }
  }
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail != 0;
}
