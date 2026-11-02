// Texture scenes for e2e_main.cpp. Included inside its anonymous namespace, after the helpers and before make_scenes().
//
// Every expectation is built from the GX texture format definitions, never by calling the renderer's decoder: the
// encoders below produce guest-memory bytes (big-endian, tiled), and the references expand a stored code the way the
// format is defined (5 bits -> 8 as (v<<3)|(v>>2), 6 -> 8 as (v<<2)|(v>>4), 4 -> 8 as v*17, 3 -> 8 as
// (v<<5)|(v<<2)|(v>>1); intensity formats replicate I into R, G, B; IA8 stores alpha first, then intensity).
//
// What Twilight Princess uses (checked in src/): CMPR for most model textures (J3D), the plain formats for UI and
// effects, C8 + RGB5A3 palettes from guest memory (d_a_mant.cpp, m_Do_lib.cpp, d_meter2_info.cpp), and the map code
// (d_map_path.cpp) which copies the EFB as GX_CTF_R8 and samples the copy as a C8 index texture. The wrap modes and
// mip chains come with every J3D texture.
#pragma once

using Rgba = std::array<int, 4>;

constexpr int expand3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
constexpr int expand4(int v) { return v * 17; }
constexpr int expand5(int v) { return (v << 3) | (v >> 2); }
constexpr int expand6(int v) { return (v << 2) | (v >> 4); }

// ---- stored code -> colour (the format definitions) ------------------------------------------------------------------
Rgba rgba_i4(unsigned raw) { const int i = expand4(raw & 15); return {i, i, i, i}; }
Rgba rgba_i8(unsigned raw) { const int i = static_cast<int>(raw & 255); return {i, i, i, i}; }
Rgba rgba_ia4(unsigned raw) { const int i = expand4(raw & 15); return {i, i, i, expand4((raw >> 4) & 15)}; }
Rgba rgba_ia8(unsigned raw) { const int i = static_cast<int>(raw & 255); return {i, i, i, static_cast<int>(raw >> 8)}; }
Rgba rgba_rgb565(unsigned raw) {
  return {expand5((raw >> 11) & 31), expand6((raw >> 5) & 63), expand5(raw & 31), 255};
}
Rgba rgba_rgb5a3(unsigned raw) {
  if ((raw & 0x8000) != 0) { // opaque: 1 | R5 G5 B5
    return {expand5((raw >> 10) & 31), expand5((raw >> 5) & 31), expand5(raw & 31), 255};
  }
  return {expand4((raw >> 8) & 15), expand4((raw >> 4) & 15), expand4(raw & 15), expand3((raw >> 12) & 7)};
}

// ---- texel patterns: asymmetric in x and y, so a transposed, mirrored or mis-tiled decode shows ----------------------
unsigned raw_i4(int x, int y) { return static_cast<unsigned>(3 * x + 5 * y + 1) & 15; }
unsigned raw_i8(int x, int y) { return static_cast<unsigned>(x * 29 + y * 53 + 7) & 255; }
unsigned raw_ia4(int x, int y) {
  return ((static_cast<unsigned>(x * 5 + y * 3 + 2) & 15) << 4) | (static_cast<unsigned>(x * 3 + y * 7 + 9) & 15);
}
unsigned raw_ia8(int x, int y) {
  return ((static_cast<unsigned>(x * 37 + y * 11 + 20) & 255) << 8) | (static_cast<unsigned>(x * 19 + y * 61 + 3) & 255);
}
unsigned raw_rgb565(int x, int y) {
  return ((static_cast<unsigned>(x * 5 + y * 3) & 31) << 11) | ((static_cast<unsigned>(x * 9 + y * 7) & 63) << 5) |
         (static_cast<unsigned>(x * 3 + y * 11 + 5) & 31);
}
unsigned raw_rgb5a3(int x, int y) {
  if (((x + y) & 1) == 0) { // opaque texels on a checkerboard, translucent ones between
    return 0x8000u | ((static_cast<unsigned>(x * 5 + y * 3) & 31) << 10) | ((static_cast<unsigned>(x * 9 + y * 7) & 31) << 5) |
           (static_cast<unsigned>(x * 3 + y * 11 + 5) & 31);
  }
  return ((static_cast<unsigned>(x + 2 * y) & 7) << 12) | ((static_cast<unsigned>(x * 3 + y) & 15) << 8) |
         ((static_cast<unsigned>(x + 5 * y) & 15) << 4) | (static_cast<unsigned>(7 * x + y) & 15);
}

struct TexFormat {
  const char* name;
  GXTexFmt fmt;
  int bits;         // per stored texel
  int tileW, tileH; // texels per tile (every tile is 32 bytes)
  unsigned (*raw)(int x, int y);
  Rgba (*rgba)(unsigned raw);
};
const TexFormat kPlainFormats[] = {
    {"i4", GX_TF_I4, 4, 8, 8, raw_i4, rgba_i4},
    {"i8", GX_TF_I8, 8, 8, 4, raw_i8, rgba_i8},
    {"ia4", GX_TF_IA4, 8, 8, 4, raw_ia4, rgba_ia4},
    {"ia8", GX_TF_IA8, 16, 4, 4, raw_ia8, rgba_ia8},
    {"rgb565", GX_TF_RGB565, 16, 4, 4, raw_rgb565, rgba_rgb565},
    {"rgb5a3", GX_TF_RGB5A3, 16, 4, 4, raw_rgb5a3, rgba_rgb5a3},
};

// Tiles left to right, top to bottom; texels row-major inside a tile; 4-bit texels high nibble first; 16-bit texels
// big-endian. Texels beyond the picture (partial tiles) are zero.
std::vector<uint8_t> encode_tiled(int bits, int tileW, int tileH, int w, int h, const std::function<unsigned(int, int)>& raw) {
  const int tilesX = (w + tileW - 1) / tileW, tilesY = (h + tileH - 1) / tileH;
  std::vector<uint8_t> out(static_cast<size_t>(tilesX) * tilesY * 32, 0);
  size_t tile = 0;
  for (int ty = 0; ty < tilesY; ++ty) {
    for (int tx = 0; tx < tilesX; ++tx, ++tile) {
      uint8_t* base = &out[tile * 32];
      int n = 0;
      for (int y = 0; y < tileH; ++y) {
        for (int x = 0; x < tileW; ++x, ++n) {
          const int px = tx * tileW + x, py = ty * tileH + y;
          const unsigned v = (px < w && py < h) ? raw(px, py) : 0;
          if (bits == 4) {
            base[n / 2] = static_cast<uint8_t>(base[n / 2] | ((n & 1) == 0 ? (v & 15) << 4 : (v & 15)));
          } else if (bits == 8) {
            base[n] = static_cast<uint8_t>(v);
          } else {
            base[2 * n] = static_cast<uint8_t>(v >> 8);
            base[2 * n + 1] = static_cast<uint8_t>(v);
          }
        }
      }
    }
  }
  return out;
}

// GX wants 32-byte aligned texture memory; the holder owns the bytes for as long as the scene closure lives.
struct TexData {
  std::vector<uint8_t> storage;
  uint8_t* data = nullptr;
  size_t size = 0;
  explicit TexData(const std::vector<uint8_t>& bytes) : storage(bytes.size() + 32), size(bytes.size()) {
    data = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 31) & ~static_cast<uintptr_t>(31));
    std::memcpy(data, bytes.data(), bytes.size());
  }
  TexData(const TexData&) = delete;
  TexData& operator=(const TexData&) = delete;
};

// ---- GX state helpers (objects live in pools: they must outlive the FIFO drain, and a scene may bind several) --------
GXTexObj& tex_obj(int i) { static GXTexObj pool[8]; return pool[i]; }
GXTlutObj& tlut_obj(int i) { static GXTlutObj pool[4]; return pool[i]; }

// One REPLACE stage on texture map 0 (POS + CLR0 + TEX0 vertices); `blend` = src-alpha blending over the clear colour.
void tex_stage_state(bool blend) {
  base_state();
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  if (blend) {
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  }
}

void load_tex(int slot, const void* data, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode ws = GX_CLAMP,
              GXTexWrapMode wt = GX_CLAMP, bool mip = false, GXTexFilter minF = GX_NEAR, GXTexFilter magF = GX_NEAR,
              f32 maxLod = 0.f) {
  GXTexObj& obj = tex_obj(slot);
  GXInitTexObj(&obj, data, w, h, fmt, ws, wt, mip ? GX_TRUE : GX_FALSE);
  GXInitTexObjLOD(&obj, minF, magF, 0.f, maxLod, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&obj, GX_TEXMAP0);
}

void load_tex_ci(int slot, const void* data, u16 w, u16 h, GXCITexFmt fmt) {
  GXTexObj& obj = tex_obj(slot);
  GXInitTexObjCI(&obj, data, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
  GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&obj, GX_TEXMAP0);
}

void uv_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 t0, f32 s1, f32 t1) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  const f32 pts[4][4] = {{x0, y0, s0, t0}, {x1, y0, s1, t0}, {x1, y1, s1, t1}, {x0, y1, s0, t1}};
  for (const auto& p : pts) {
    GXPosition3f32(p[0], p[1], -1.f);
    GXColor4u8(255, 255, 255, 255);
    GXTexCoord2f32(p[2], p[3]);
  }
  GXEnd();
}

// ---- checks ---------------------------------------------------------------------------------------------------------
// A translucent texel, as it must look after src-alpha blending over the clear colour.
std::array<int, 3> over_gray(const Rgba& t) {
  const double a = t[3] / 255.0;
  return {static_cast<int>(t[0] * a + kGray * (1 - a) + 0.5), static_cast<int>(t[1] * a + kGray * (1 - a) + 0.5),
          static_cast<int>(t[2] * a + kGray * (1 - a) + 0.5)};
}

// Every texel centre of a w x h texture drawn `scale` pixels per texel with its top-left corner at (x0, y0).
void check_texel_grid(const Image& img, Checker& c, int x0, int y0, int w, int h, int scale,
                      const std::function<Rgba(int, int)>& expected, const std::function<int(int, int)>& tolerance) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto e = over_gray(expected(x, y));
      if (!c.px(img, x0 + scale * x + scale / 2, y0 + scale * y + scale / 2, e[0], e[1], e[2], tolerance(x, y))) {
        std::printf("    (texel %d,%d)\n", x, y);
      }
    }
  }
}

// ---- compressed (CMPR / DXT1) ----------------------------------------------------------------------------------------
// A 4x4 block: two RGB565 endpoints and 2-bit indices (row 0 is the top row; the leftmost texel is in the high bits).
// c0 > c1: four colours, index 2 = (2*c0 + c1)/3 and 3 = (c0 + 2*c1)/3. c0 <= c1: three colours, index 2 = (c0 + c1)/2
// and index 3 = transparent black. CMPR stores 8x8 tiles of four blocks in the order top-left, top-right,
// bottom-left, bottom-right.
struct DxtBlock {
  unsigned c0, c1;
  uint8_t idx[4][4];
};
constexpr unsigned rgb565_of(unsigned r5, unsigned g6, unsigned b5) { return (r5 << 11) | (g6 << 5) | b5; }

Rgba dxt_texel(const DxtBlock& b, int x, int y) {
  const Rgba c0 = rgba_rgb565(b.c0), c1 = rgba_rgb565(b.c1);
  const int i = b.idx[y][x];
  if (i == 0) return c0;
  if (i == 1) return c1;
  const auto mix = [&](double w0) {
    return Rgba{static_cast<int>(c0[0] * w0 + c1[0] * (1 - w0) + 0.5), static_cast<int>(c0[1] * w0 + c1[1] * (1 - w0) + 0.5),
                static_cast<int>(c0[2] * w0 + c1[2] * (1 - w0) + 0.5), 255};
  };
  if (b.c0 > b.c1) {
    return i == 2 ? mix(2.0 / 3.0) : mix(1.0 / 3.0);
  }
  return i == 2 ? mix(0.5) : Rgba{0, 0, 0, 0};
}

std::vector<uint8_t> encode_cmpr(const DxtBlock (&blocks)[2][4]) { // 16 x 8 texels: 2 tiles side by side
  std::vector<uint8_t> out;
  for (int tile = 0; tile < 2; ++tile) {
    for (int sub = 0; sub < 4; ++sub) {
      const DxtBlock& b = blocks[sub >> 1][tile * 2 + (sub & 1)];
      out.push_back(static_cast<uint8_t>(b.c0 >> 8));
      out.push_back(static_cast<uint8_t>(b.c0));
      out.push_back(static_cast<uint8_t>(b.c1 >> 8));
      out.push_back(static_cast<uint8_t>(b.c1));
      for (int y = 0; y < 4; ++y) {
        out.push_back(static_cast<uint8_t>((b.idx[y][0] << 6) | (b.idx[y][1] << 4) | (b.idx[y][2] << 2) | b.idx[y][3]));
      }
    }
  }
  return out;
}

// ---- palettes -------------------------------------------------------------------------------------------------------
// Entry i of a 256-entry palette in each TLUT format; `variant` 1 rotates the colour channels so that two palettes
// differ at (almost) every index.
unsigned tlut_rgb5a3(int i, int variant) {
  int c[3];
  if ((i & 1) == 0) {
    c[0] = (i * 7) & 31, c[1] = (i * 11 + 3) & 31, c[2] = (i * 5 + 9) & 31;
  } else {
    c[0] = (i * 3) & 15, c[1] = (i * 7 + 1) & 15, c[2] = (i * 5 + 2) & 15;
  }
  if (variant == 1) {
    std::swap(c[0], c[1]);
    std::swap(c[1], c[2]);
  }
  if ((i & 1) == 0) {
    return 0x8000u | (static_cast<unsigned>(c[0]) << 10) | (static_cast<unsigned>(c[1]) << 5) | static_cast<unsigned>(c[2]);
  }
  return (static_cast<unsigned>(4 + (i & 3)) << 12) | (static_cast<unsigned>(c[0]) << 8) | (static_cast<unsigned>(c[1]) << 4) |
         static_cast<unsigned>(c[2]);
}
unsigned tlut_rgb565(int i, int) { return rgb565_of(static_cast<unsigned>((i * 3) & 31), static_cast<unsigned>((i * 5 + 1) & 63), static_cast<unsigned>((i * 11) & 31)); }
unsigned tlut_ia8(int i, int) { return (static_cast<unsigned>(50 + i * 13) & 255) << 8 | (static_cast<unsigned>(i * 47 + 10) & 255); }

std::vector<uint8_t> encode_tlut(int entries, unsigned (*entry)(int, int), int variant) {
  std::vector<uint8_t> out;
  for (int i = 0; i < entries; ++i) {
    const unsigned v = entry(i, variant);
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

unsigned idx_c8(int x, int y) { return static_cast<unsigned>(x * 13 + y * 37 + 5) & 255; }
unsigned idx_c8_second(int x, int y) { return (idx_c8(x, y) * 7 + 101) & 255; }
unsigned idx_c4(int x, int y) { return static_cast<unsigned>(x * 3 + y * 5 + 1) & 15; }

// ---- mip chains -------------------------------------------------------------------------------------------------------
// One solid colour per level, RGBA8 in 4x4 tiles of 64 bytes (16 x AR, then 16 x GB); a level smaller than a tile still
// occupies a whole tile.
constexpr int kMipColors[6][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}};
std::vector<uint8_t> encode_solid_mips(int w, int h, int levels) {
  std::vector<uint8_t> out;
  for (int l = 0; l < levels; ++l) {
    const int lw = std::max(w >> l, 1), lh = std::max(h >> l, 1);
    const int tiles = ((lw + 3) / 4) * ((lh + 3) / 4);
    for (int t = 0; t < tiles; ++t) {
      for (int i = 0; i < 16; ++i) {
        out.push_back(255);
        out.push_back(static_cast<uint8_t>(kMipColors[l][0]));
      }
      for (int i = 0; i < 16; ++i) {
        out.push_back(static_cast<uint8_t>(kMipColors[l][1]));
        out.push_back(static_cast<uint8_t>(kMipColors[l][2]));
      }
    }
  }
  return out;
}

void add_texture_scenes(std::vector<Scene>& s) {
  // ---- CMPR -----------------------------------------------------------------------------------------------------------
  // Eight blocks in two tiles. Endpoint colours A and B differ by at most 99 per channel: Dolphin interpolates with 3/8
  // and 5/8, the S3TC definition with 1/3 and 2/3, and the largest gap between the two is 99 * (2/3 - 5/8) = 4.1 levels,
  // so interpolated texels get tolerance 6. Solid blocks, endpoints and the half-way colour must be exact.
  {
    const unsigned A = rgb565_of(22, 36, 10), B = rgb565_of(10, 22, 22);
    const unsigned RED = rgb565_of(31, 0, 0), GREEN = rgb565_of(0, 63, 0), BLUE = rgb565_of(0, 0, 31), YELLOW = rgb565_of(31, 63, 0);
    const auto solid = [](unsigned c) { return DxtBlock{c, c, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}}; };
    const auto all = [](unsigned c0, unsigned c1, uint8_t i) {
      return DxtBlock{c0, c1, {{i, i, i, i}, {i, i, i, i}, {i, i, i, i}, {i, i, i, i}}};
    };
    static const DxtBlock kBlocks[2][4] = {
        {{A, B, {{0, 1, 2, 3}, {3, 2, 1, 0}, {1, 1, 2, 2}, {3, 3, 0, 0}}},  // four colours, every index, no symmetry
         {B, A, {{0, 1, 2, 3}, {3, 2, 1, 0}, {2, 3, 0, 1}, {1, 0, 3, 2}}},  // three colours + transparent
         solid(BLUE), solid(YELLOW)},
        {solid(RED), solid(GREEN), all(A, B, 2), all(B, A, 2)},
    };
    s.push_back({"tex_cmpr", "CMPR 16x8: 8 blocks over 2 tiles (4-colour, 3-colour + transparent, solids): every texel, block order, bit order",
                 [] {
                   static const TexData data(encode_cmpr(kBlocks));
                   tex_stage_state(true);
                   load_tex(0, data.data, 16, 8, GX_TF_CMPR);
                   uv_rect(100, 100, 228, 164, 0, 0, 1, 1);
                 },
                 [](const Image& img, Checker& c) {
                   const auto texel = [](int x, int y) { return dxt_texel(kBlocks[y / 4][x / 4], x % 4, y % 4); };
                   const auto tol = [](int x, int y) {
                     const DxtBlock& b = kBlocks[y / 4][x / 4];
                     const int i = b.idx[y % 4][x % 4];
                     return (i >= 2 && b.c0 > b.c1) ? 6 : 3; // only the 1/3, 2/3 colours depend on the blend weights
                   };
                   check_texel_grid(img, c, 100, 100, 16, 8, 8, texel, tol);
                 }});
  }

  // ---- plain formats ----------------------------------------------------------------------------------------------------
  for (const TexFormat& f : kPlainFormats) {
    const auto data = std::make_shared<TexData>(
        encode_tiled(f.bits, f.tileW, f.tileH, 8, 8, [f](int x, int y) { return f.raw(x, y); }));
    const std::string name = std::string("tex_fmt_") + f.name;
    const std::string what = std::string(f.name) + " 8x8 through GL: every texel (tiling, bit order, channel expansion, alpha) vs the format definition";
    s.push_back({strdup(name.c_str()), strdup(what.c_str()),
                 [f, data] {
                   tex_stage_state(true);
                   load_tex(0, data->data, 8, 8, f.fmt);
                   uv_rect(100, 100, 164, 164, 0, 0, 1, 1);
                 },
                 [f](const Image& img, Checker& c) {
                   check_texel_grid(img, c, 100, 100, 8, 8, 8, [f](int x, int y) { return f.rgba(f.raw(x, y)); },
                                    [](int, int) { return 3; });
                 }});
  }

  // ---- wrap modes -------------------------------------------------------------------------------------------------------
  // A 4x4 texture with 16 distinct texels, s and t from -1 to 2 over a 192 x 192 quad: 16 pixels per texel, texel index
  // n = cell - 4 runs -4..7. repeat: n mod 4; mirror: m = n mod 8, texel m for m < 4 else 7 - m; clamp: n limited to 0..3.
  {
    static const GXTexWrapMode modes[3][2] = {{GX_REPEAT, GX_MIRROR}, {GX_MIRROR, GX_CLAMP}, {GX_CLAMP, GX_REPEAT}};
    const auto wrapped = [](GXTexWrapMode mode, int n) {
      switch (mode) {
      case GX_REPEAT: return ((n % 4) + 4) % 4;
      case GX_MIRROR: {
        const int m = ((n % 8) + 8) % 8;
        return m < 4 ? m : 7 - m;
      }
      default: return std::clamp(n, 0, 3);
      }
    };
    s.push_back({"tex_wrap", "repeat / mirror / clamp on each axis (3 quads, s and t from -1 to 2 over a 4x4 texture): every cell",
                 [] {
                   build_test_texture();
                   tex_stage_state(false);
                   for (int q = 0; q < 3; ++q) {
                     load_tex(q, gTexRgba8, 4, 4, GX_TF_RGBA8, modes[q][0], modes[q][1]);
                     const f32 x0 = 8.f + 216.f * static_cast<f32>(q);
                     uv_rect(x0, 100, x0 + 192, 292, -1, -1, 2, 2);
                   }
                 },
                 [=](const Image& img, Checker& c) {
                   for (int q = 0; q < 3; ++q) {
                     for (int cy = 0; cy < 12; ++cy) {
                       for (int cx = 0; cx < 12; ++cx) {
                         const int tx = wrapped(modes[q][0], cx - 4), ty = wrapped(modes[q][1], cy - 4);
                         const auto e = test_texel(tx, ty);
                         if (!c.px(img, 8 + 216 * q + 16 * cx + 8, 100 + 16 * cy + 8, e[0], e[1], e[2], 2)) {
                           std::printf("    (quad %d, cell %d,%d -> texel %d,%d)\n", q, cx, cy, tx, ty);
                         }
                       }
                     }
                   }
                 }});
  }

  // ---- mipmaps ----------------------------------------------------------------------------------------------------------
  // 32x32 with a different solid colour per level (red, green, blue, yellow, magenta, cyan = levels 0..5). A quad of size
  // p pixels with the whole texture on it shows 32/p texels per pixel, so LOD = log2(32/p): 32, 16, 8, 4, 2, 1 px select
  // levels 0..5 (exact powers of two: nowhere near the rounding boundary of nearest-level selection).
  {
    static constexpr int kQuadX[6] = {20, 80, 120, 150, 175, 195}, kQuadSize[6] = {32, 16, 8, 4, 2, 1};
    const auto draw_quads = [] {
      for (int q = 0; q < 6; ++q) {
        uv_rect(static_cast<f32>(kQuadX[q]), 100, static_cast<f32>(kQuadX[q] + kQuadSize[q]),
                static_cast<f32>(100 + kQuadSize[q]), 0, 0, 1, 1);
      }
    };
    const auto check_levels = [](const int (&level)[6]) {
      return [level](const Image& img, Checker& c) {
        for (int q = 0; q < 6; ++q) {
          const int* col = kMipColors[level[q]];
          c.px(img, kQuadX[q] + kQuadSize[q] / 2, 100 + kQuadSize[q] / 2, col[0], col[1], col[2], 2);
        }
      };
    };
    static const int kFull[6] = {0, 1, 2, 3, 4, 5}, kFlat[6] = {0, 0, 0, 0, 0, 0}, kClamped[6] = {0, 1, 2, 2, 2, 2};
    s.push_back({"tex_mip", "32x32 RGBA8 with 6 solid-colour levels, nearest-mip: quads of 32/16/8/4/2/1 px show levels 0..5",
                 [draw_quads] {
                   static const TexData data(encode_solid_mips(32, 32, 6));
                   tex_stage_state(false);
                   load_tex(0, data.data, 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, true, GX_NEAR_MIP_NEAR, GX_NEAR, 5.f);
                   draw_quads();
                 },
                 check_levels(kFull)});
    s.push_back({"tex_mip_off", "a mip filter (NEAR_MIP_NEAR) on a texture without the mipmap flag: only level 0 exists, every quad shows it (an incomplete GL texture would sample black)",
                 [draw_quads] {
                   static const TexData data(encode_solid_mips(32, 32, 6));
                   tex_stage_state(false);
                   load_tex(0, data.data, 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, false, GX_NEAR_MIP_NEAR, GX_NEAR, 0.f);
                   draw_quads();
                 },
                 check_levels(kFlat)});
    s.push_back({"tex_mip_truncated", "only levels 0..2 in memory: smaller quads show level 2 and nothing is read past the data (solid levels cannot tell a max-LOD clamp from a chain completed off level 2; the clamp itself is not implemented)",
                 [draw_quads] {
                   static const TexData data(encode_solid_mips(32, 32, 3));
                   tex_stage_state(false);
                   load_tex(0, data.data, 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, true, GX_NEAR_MIP_NEAR, GX_NEAR, 2.f);
                   draw_quads();
                 },
                 check_levels(kClamped)});
  }

  // ---- palettes: guest memory (TP: d_a_mant.cpp, m_Do_lib.cpp, d_meter2_info.cpp use C8 + RGB5A3) ------------------------
  struct PaletteCase {
    const char* name;
    const char* what;
    bool fourBit;
    GXTlutFmt tlutFmt;
    int entries;
    unsigned (*entry)(int, int);
    Rgba (*rgba)(unsigned);
  };
  const PaletteCase kPalettes[] = {
      {"tex_pal_c8_rgb5a3", "(TP's combination) C8 16x8 + 256-entry RGB5A3 TLUT: every texel", false, GX_TL_RGB5A3, 256, tlut_rgb5a3, rgba_rgb5a3},
      {"tex_pal_c8_rgb565", "C8 16x8 + 256-entry RGB565 TLUT: every texel", false, GX_TL_RGB565, 256, tlut_rgb565, rgba_rgb565},
      {"tex_pal_c4_ia8", "C4 16x8 + 16-entry IA8 TLUT: every texel", true, GX_TL_IA8, 16, tlut_ia8, rgba_ia8},
  };
  for (const PaletteCase& p : kPalettes) {
    const auto idx = p.fourBit ? idx_c4 : idx_c8;
    const auto texData = std::make_shared<TexData>(
        p.fourBit ? encode_tiled(4, 8, 8, 16, 8, idx) : encode_tiled(8, 8, 4, 16, 8, idx));
    const auto tlutData = std::make_shared<TexData>(encode_tlut(p.entries, p.entry, 0));
    s.push_back({p.name, p.what,
                 [p, texData, tlutData] {
                   tex_stage_state(true);
                   GXInitTlutObj(&tlut_obj(0), tlutData->data, p.tlutFmt, static_cast<u16>(p.entries));
                   GXLoadTlut(&tlut_obj(0), GX_TLUT0);
                   load_tex_ci(0, texData->data, 16, 8, p.fourBit ? GX_TF_C4 : GX_TF_C8);
                   uv_rect(100, 100, 228, 164, 0, 0, 1, 1);
                 },
                 [p, idx](const Image& img, Checker& c) {
                   check_texel_grid(img, c, 100, 100, 16, 8, 8, [&](int x, int y) { return p.rgba(p.entry(static_cast<int>(idx(x, y)), 0)); },
                                    [](int, int) { return 3; });
                 }});
  }

  // d_a_mant.cpp: the same C8 image is drawn with two different TLUT objects loaded into the same slot, and the TLUT
  // object is also re-pointed at new data (GXInitTlutObjData) between draws. Each draw must use the palette that was
  // loaded when it was issued, although the whole frame is recorded before anything is replayed.
  s.push_back({"tex_pal_two_tluts", "(d_a_mant.cpp's pattern) one C8 image drawn with TLUT A, then B, in the same slot, then with one TLUT object re-pointed from A to B",
               [] {
                 static const TexData image(encode_tiled(8, 8, 4, 16, 8, idx_c8));
                 static const TexData tlutA(encode_tlut(256, tlut_rgb5a3, 0)), tlutB(encode_tlut(256, tlut_rgb5a3, 1));
                 tex_stage_state(true);
                 load_tex_ci(0, image.data, 16, 8, GX_TF_C8);
                 GXInitTlutObj(&tlut_obj(0), tlutA.data, GX_TL_RGB5A3, 256);
                 GXInitTlutObj(&tlut_obj(1), tlutB.data, GX_TL_RGB5A3, 256);
                 GXLoadTlut(&tlut_obj(0), GX_TLUT0);
                 uv_rect(40, 100, 168, 164, 0, 0, 1, 1); // palette A
                 GXLoadTlut(&tlut_obj(1), GX_TLUT0);
                 uv_rect(300, 100, 428, 164, 0, 0, 1, 1); // palette B
                 GXInitTlutObj(&tlut_obj(2), tlutA.data, GX_TL_RGB5A3, 256);
                 GXLoadTlut(&tlut_obj(2), GX_TLUT0);
                 uv_rect(40, 260, 168, 324, 0, 0, 1, 1); // palette A through object 2
                 GXInitTlutObjData(&tlut_obj(2), tlutB.data);
                 GXLoadTlut(&tlut_obj(2), GX_TLUT0);
                 uv_rect(300, 260, 428, 324, 0, 0, 1, 1); // the same object, now palette B
               },
               [](const Image& img, Checker& c) {
                 const auto pal = [](int variant) {
                   return [variant](int x, int y) { return rgba_rgb5a3(tlut_rgb5a3(static_cast<int>(idx_c8(x, y)), variant)); };
                 };
                 const auto tol = [](int, int) { return 3; };
                 check_texel_grid(img, c, 40, 100, 16, 8, 8, pal(0), tol);
                 check_texel_grid(img, c, 300, 100, 16, 8, 8, pal(1), tol);
                 check_texel_grid(img, c, 40, 260, 16, 8, 8, pal(0), tol);
                 check_texel_grid(img, c, 300, 260, 16, 8, 8, pal(1), tol);
               }});

  // d_map_path.cpp: render index values into the EFB (red = index), copy them as GX_CTF_R8, then sample the copy as a C8
  // image through a TLUT. The port turns that into a GPU pass (a palette-derived texture).
  struct EfbPalette {
    bool reloadTlut = false; // re-point the TLUT object at palette B and draw the same copy again
    bool rewrite = false;    // render a second index pattern, copy it over the same buffer and draw that too
  };
  const auto draw_index_quads = [](unsigned (*idx)(int, int)) {
    base_state();
    GXBegin(GX_QUADS, GX_VTXFMT0, 16 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 16; ++x) {
        for (const auto& [dx, dy] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 1}}) {
          GXPosition3f32(static_cast<f32>(x + dx), static_cast<f32>(y + dy), -1.f);
          GXColor4u8(static_cast<u8>(idx(x, y)), 0, 0, 255);
        }
      }
    }
    GXEnd();
  };
  const auto efb_palette_scene = [draw_index_quads](EfbPalette p, uint8_t* copyBuf) {
    return [=] {
      static const TexData tlutA(encode_tlut(256, tlut_rgb5a3, 0)), tlutB(encode_tlut(256, tlut_rgb5a3, 1));
      const auto copy_indices = [&](unsigned (*idx)(int, int)) {
        draw_index_quads(idx);
        GXSetTexCopySrc(0, 0, 16, 8);
        GXSetTexCopyDst(16, 8, GX_CTF_R8, GX_FALSE);
        GXCopyTex(copyBuf, GX_FALSE);
        rect(0, 0, 32, 16, 2, kGray, kGray, kGray); // wipe the index pattern: the indices must come from the copy
      };
      copy_indices(idx_c8);
      tex_stage_state(true);
      GXInitTlutObj(&tlut_obj(0), tlutA.data, GX_TL_RGB5A3, 256);
      GXLoadTlut(&tlut_obj(0), GX_TLUT0);
      load_tex_ci(0, copyBuf, 16, 8, GX_TF_C8);
      uv_rect(100, 100, 228, 164, 0, 0, 1, 1); // palette A, first indices
      if (p.reloadTlut) {
        GXInitTlutObjData(&tlut_obj(0), tlutB.data);
        GXLoadTlut(&tlut_obj(0), GX_TLUT0);
        uv_rect(300, 100, 428, 164, 0, 0, 1, 1); // the same TLUT object, now palette B
      }
      if (p.rewrite) {
        copy_indices(idx_c8_second); // the same buffer address, new contents (d_map_path.cpp rewrites its copy every frame)
        tex_stage_state(true);
        GXLoadTlut(&tlut_obj(0), GX_TLUT0);
        load_tex_ci(0, copyBuf, 16, 8, GX_TF_C8);
        uv_rect(300, 100, 428, 164, 0, 0, 1, 1); // palette A, second indices
      }
    };
  };
  const auto efb_palette_check = [](EfbPalette p) {
    return [p](const Image& img, Checker& c) {
      const auto pal = [](int variant, unsigned (*idx)(int, int)) {
        return [variant, idx](int x, int y) { return rgba_rgb5a3(tlut_rgb5a3(static_cast<int>(idx(x, y)), variant)); };
      };
      const auto tol = [](int, int) { return 3; };
      check_texel_grid(img, c, 100, 100, 16, 8, 8, pal(0, idx_c8), tol);
      if (p.reloadTlut) {
        check_texel_grid(img, c, 300, 100, 16, 8, 8, pal(1, idx_c8), tol);
      }
      if (p.rewrite) {
        check_texel_grid(img, c, 300, 100, 16, 8, 8, pal(0, idx_c8_second), tol);
      }
      c.px(img, 8, 4, kGray, kGray, kGray); // the index pattern is gone from where it was rendered: only the copy remains
    };
  };
  static uint8_t sCopyBuf[3][16 * 8]; // only the addresses matter: each copy lives on the GPU
  s.push_back({"tex_pal_efb_copy", "(d_map_path.cpp's pattern) EFB indices copied as GX_CTF_R8, sampled as C8 through an RGB5A3 TLUT: every texel",
               efb_palette_scene({}, sCopyBuf[0]), efb_palette_check({})});
  s.push_back({"tex_pal_efb_copy_retlut", "the same, then the TLUT object is re-pointed at new data and the copy is drawn again: each draw keeps its own palette",
               efb_palette_scene({.reloadTlut = true}, sCopyBuf[1]), efb_palette_check({.reloadTlut = true})});
  s.push_back({"tex_pal_efb_copy_rewrite", "the same copy buffer is rewritten with different indices between two draws: the second draw sees the new indices, the first keeps the old",
               efb_palette_scene({.rewrite = true}, sCopyBuf[2]), efb_palette_check({.rewrite = true})});
}
