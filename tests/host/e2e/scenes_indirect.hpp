// Indirect-texturing (TEV) scenes for e2e_main.cpp. Included inside its anonymous namespace, after scenes_texture.hpp.
//
// What Twilight Princess does (checked in src/ and libs/): JPAResource::setPTev (particles with an indirect "ETS" stage)
// issues GXSetIndTexOrder(IND0, TEXCOORD1, TEXMAP2), GXSetIndTexCoordScale(IND0, ITS_1, ITS_1), GXSetIndTexMtx(ITM_0,
// the particle's 2x3 matrix, its scale exponent) and GXSetTevIndirect(TEVSTAGE0, IND0, ITF_8, ITB_STU, ITM_0, ITW_OFF,
// ITW_OFF, no add-prev, no LOD, ITBA_OFF); daYkgr_c's callback re-loads ITM_0 with {{0.5,0,0},{0,0.5,0}}. J2D and J3D
// materials load whatever format, bias, matrix, scale, wrap and add-prev the model file carries (J2DIndTevStage,
// J3DIndBlockFull), so the other scenes cover that parameter space, not a known TP use. No disc is available here:
// which of those combinations the game's models contain is unknown.
//
// The expectation is an independent model of the hardware's indirect pipeline, written from Dolphin's pixel shader
// generator (VideoCommon/PixelShaderGen.cpp, WriteStage and the indirect-stage loop) and PixelShaderManager's matrix
// constants, never from the renderer's shader:
//   * the indirect texel is sampled at (texcoord in 1/128 texel) >> scale, nearest, and read as (S, T, U) = (A, B, G);
//   * crd = byte >> {0, 3, 4, 5} for ITF_8/5/4/3, plus a bias of -128 (ITF_8) or +1 on the biased components;
//   * trans = (row . crd) >> 3, then shifted right by -scale_exp (left when positive), in 1/128 texel; rows hold the
//     matrix as trunc(m * 1024) (11-bit signed);
//   * the base coordinate is wrapped to 256..16 texels (ITW_256..ITW_16), zeroed (ITW_0) or left alone (ITW_OFF),
//     then trans is added (and, with add-prev, the previous stage's coordinate);
//   * the direct texture is then sampled nearest at that coordinate with the map's wrap mode (clamp here).
// A texel is only checked where the sampling position lies at least 12/128 texel inside a texel: the renderer works
// in floating point, so the last bits of the 1/128 quantisation are not pinned, only which texel is fetched.
#pragma once

constexpr int kIndW = 64, kIndH = 16; // direct and indirect textures: same size, so a texcoord is 1:1 in texels
constexpr int kIndX = 100, kIndY = 100;

struct IndCfg {
  const char* name;
  const char* what;
  GXIndTexFormat fmt = GX_ITF_8;
  GXIndTexBiasSel bias = GX_ITB_STU;
  f32 mtx[2][3] = {{0.5f, 0.f, 0.f}, {0.f, 0.5f, 0.f}};
  int exp = 0;
  GXIndTexScale scaleS = GX_ITS_1, scaleT = GX_ITS_1;
  GXIndTexWrap wrapS = GX_ITW_OFF, wrapT = GX_ITW_OFF;
  int lo = 104, hi = 151; // range of the three indirect bytes (S = A, T = B, U = G)
};

struct IndByte {
  int s, t, u;
};

int ind_fmt_shift(GXIndTexFormat f) { return f == GX_ITF_8 ? 0 : f == GX_ITF_5 ? 3 : f == GX_ITF_4 ? 4 : 5; }
int ind_mtx_raw(f32 m) { return std::clamp(static_cast<int>(m * 1024.f), -1024, 1023); }

// The offset an indirect texel adds to the coordinate, in 1/128 texel.
std::array<int, 2> ind_trans(const IndCfg& c, const IndByte& b) {
  const int shift = ind_fmt_shift(c.fmt);
  int crd[3] = {b.s >> shift, b.t >> shift, b.u >> shift};
  const int add = c.fmt == GX_ITF_8 ? -128 : 1;
  for (int k = 0; k < 3; ++k) {
    if (((static_cast<int>(c.bias) >> k) & 1) != 0) {
      crd[k] += add;
    }
  }
  std::array<int, 2> t{};
  for (int r = 0; r < 2; ++r) {
    int dot = 0;
    for (int k = 0; k < 3; ++k) {
      dot += ind_mtx_raw(c.mtx[r][k]) * crd[k];
    }
    dot >>= 3;
    t[r] = c.exp <= 0 ? dot >> -c.exp : dot << c.exp;
  }
  return t;
}

// The base coordinate (1/128 texel) after the stage's wrap setting.
int ind_wrap(GXIndTexWrap w, int f) {
  if (w == GX_ITW_OFF) return f;
  if (w == GX_ITW_0) return 0;
  return f & (((256 >> (static_cast<int>(w) - GX_ITW_256)) << 7) - 1);
}
bool ind_inside_texel(int coord) { return (coord & 127) >= 12 && (coord & 127) <= 116; }

unsigned ind_hash(unsigned a, unsigned b, unsigned c) {
  unsigned v = a * 374761393u + b * 668265263u + c * 2246822519u;
  v = (v ^ (v >> 13)) * 1274126177u;
  return v ^ (v >> 16);
}

// Indirect bytes for texel (ix, iy): pseudo-random inside the config's range, redrawn until the resulting sampling
// position (the pixel centre's 64/128 fraction plus `extra`) is clear of texel edges on both axes.
IndByte ind_pick(const IndCfg& c, int ix, int iy, unsigned salt, std::array<int, 2> extra = {0, 0}) {
  const int span = c.hi - c.lo + 1;
  for (unsigned k = 0; k < 100000; ++k) {
    const unsigned base = salt * 65536u + k * 4;
    const IndByte b{c.lo + static_cast<int>(ind_hash(ix, iy, base + 0) % span),
                    c.lo + static_cast<int>(ind_hash(ix, iy, base + 1) % span),
                    c.lo + static_cast<int>(ind_hash(ix, iy, base + 2) % span)};
    const auto t = ind_trans(c, b);
    if (ind_inside_texel(ind_wrap(c.wrapS, 64) + extra[0] + t[0]) && ind_inside_texel(ind_wrap(c.wrapT, 64) + extra[1] + t[1])) {
      return b;
    }
  }
  std::abort(); // the scene's parameters leave no valid indirect value: fix the scene
}

Rgba ind_direct_texel(int x, int y) { return {4 * x, 16 * y, (x * 5 + y * 9) & 255, 255}; }

// GX_TF_RGBA8: 4x4 tiles of 64 bytes, a plane of (A, R) pairs then a plane of (G, B) pairs.
std::vector<uint8_t> encode_rgba8_tiled(int w, int h, const std::function<Rgba(int, int)>& px) {
  std::vector<uint8_t> out;
  for (int ty = 0; ty < h / 4; ++ty) {
    for (int tx = 0; tx < w / 4; ++tx) {
      for (int plane = 0; plane < 2; ++plane) {
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const Rgba c = px(tx * 4 + x, ty * 4 + y);
            out.push_back(static_cast<uint8_t>(plane == 0 ? c[3] : c[1]));
            out.push_back(static_cast<uint8_t>(plane == 0 ? c[0] : c[2]));
          }
        }
      }
    }
  }
  return out;
}

// An indirect texture holding `table` as (R = noise, G = U, B = T, A = S).
std::vector<uint8_t> encode_indirect(const std::vector<IndByte>& table) {
  return encode_rgba8_tiled(kIndW, kIndH, [&](int x, int y) {
    const IndByte& b = table[static_cast<size_t>(y) * kIndW + x];
    return Rgba{static_cast<int>(ind_hash(x, y, 0xabcdu) & 255), b.u, b.t, b.s};
  });
}

void ind_load_map(int slot, GXTexMapID map, const uint8_t* data) {
  GXTexObj& obj = tex_obj(slot);
  GXInitTexObj(&obj, data, kIndW, kIndH, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&obj, map);
}

// POS + CLR0 + TEX0 vertices, two texgens that both read TEX0 (TEXCOORD0 for the direct stage, TEXCOORD1 for the
// indirect lookup, as JPAResource::setPTev arranges them).
void ind_base_state() {
  base_state();
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetNumTexGens(2);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
}

void ind_quad() { uv_rect(kIndX, kIndY, kIndX + kIndW, kIndY + kIndH, 0, 0, 1, 1); }

// The direct texel a pixel must show, given the coordinate (1/128 texel) its TEV stage ends up with.
void ind_check_pixel(const Image& img, Checker& c, int x, int y, int cx, int cy) {
  const Rgba e = ind_direct_texel(std::clamp(cx >> 7, 0, kIndW - 1), std::clamp(cy >> 7, 0, kIndH - 1));
  c.px(img, kIndX + x, kIndY + y, e[0], e[1], e[2], 1);
}

void add_indirect_scenes(std::vector<Scene>& s) {
  static const IndCfg kScenes[] = {
      {.name = "tev_ind_warp",
       .what = "(TP: JPAResource::setPTev) ITF_8, bias STU, matrix 0.5 diagonal, exp 0: pixel (x, y) shows direct texel "
               "(x + (S-128)/2, y + (T-128)/2), clamped at the edges"},
      {.name = "tev_ind_matrix",
       .what = "full 2x3 matrix with U terms and scale exponent +1: every pixel shows the texel the matrix product selects",
       .mtx = {{0.25f, -0.125f, 0.0625f}, {0.125f, 0.25f, -0.25f}},
       .exp = 1},
      {.name = "tev_ind_exp_neg",
       .what = "matrix with negative terms and scale exponent -2 (shift right, flooring)",
       .mtx = {{0.75f, 0.5f, -0.5f}, {-0.5f, 0.75f, 0.25f}},
       .exp = -2},
      {.name = "tev_ind_coord_scale",
       .what = "GXSetIndTexCoordScale(IND0, ITS_2, ITS_4): the indirect texture is magnified 2x in s and 4x in t",
       .scaleS = GX_ITS_2,
       .scaleT = GX_ITS_4},
      {.name = "tev_ind_itf5",
       .what = "ITF_5 (5-bit indirect values, +1 bias): crd = (byte >> 3) + 1",
       .fmt = GX_ITF_5,
       .mtx = {{0.25f, -0.125f, 0.0625f}, {0.125f, 0.25f, -0.25f}},
       .lo = 0,
       .hi = 255},
      {.name = "tev_ind_itf4",
       .what = "ITF_4 (4-bit indirect values, +1 bias): crd = (byte >> 4) + 1",
       .fmt = GX_ITF_4,
       .mtx = {{0.5f, -0.25f, 0.125f}, {0.25f, 0.5f, -0.125f}},
       .lo = 0,
       .hi = 255},
      {.name = "tev_ind_itf3",
       .what = "ITF_3 (3-bit indirect values, +1 bias): crd = (byte >> 5) + 1",
       .fmt = GX_ITF_3,
       .mtx = {{0.75f, -0.5f, 0.25f}, {0.5f, 0.75f, -0.25f}},
       .lo = 0,
       .hi = 255},
      {.name = "tev_ind_bias_st",
       .what = "bias on S and T only: U enters the matrix product unbiased",
       .bias = GX_ITB_ST,
       .mtx = {{0.25f, -0.125f, 0.015625f}, {0.125f, 0.25f, -0.015625f}}},
      {.name = "tev_ind_wrap",
       .what = "ITW_16 on s and ITW_0 on t: the base column repeats every 16 texels and the row comes from the indirect offset alone",
       .wrapS = GX_ITW_16,
       .wrapT = GX_ITW_0},
  };

  // One-stage scenes: the direct texture on map 0, the indirect one on map 2 (TP's arrangement).
  for (const IndCfg& cfg : kScenes) {
    auto table = std::make_shared<std::vector<IndByte>>(static_cast<size_t>(kIndW) * kIndH);
    for (int y = 0; y < kIndH; ++y) {
      for (int x = 0; x < kIndW; ++x) {
        (*table)[static_cast<size_t>(y) * kIndW + x] = ind_pick(cfg, x, y, 0);
      }
    }
    auto direct = std::make_shared<TexData>(encode_rgba8_tiled(kIndW, kIndH, ind_direct_texel));
    auto indirect = std::make_shared<TexData>(encode_indirect(*table));
    s.push_back({cfg.name, cfg.what,
                 [&cfg, direct, indirect] {
                   ind_base_state();
                   ind_load_map(0, GX_TEXMAP0, direct->data);
                   ind_load_map(1, GX_TEXMAP2, indirect->data);
                   GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP2);
                   GXSetIndTexCoordScale(GX_INDTEXSTAGE0, cfg.scaleS, cfg.scaleT);
                   f32 mtx[2][3];
                   std::memcpy(mtx, cfg.mtx, sizeof(mtx));
                   GXSetIndTexMtx(GX_ITM_0, mtx, static_cast<s8>(cfg.exp));
                   GXSetNumIndStages(1);
                   GXSetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, cfg.fmt, cfg.bias, GX_ITM_0, cfg.wrapS, cfg.wrapT, GX_FALSE,
                                    GX_FALSE, GX_ITBA_OFF);
                   GXSetNumTevStages(1);
                   GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                   GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
                   ind_quad();
                 },
                 [&cfg, table](const Image& img, Checker& c) {
                   for (int y = 0; y < kIndH; ++y) {
                     for (int x = 0; x < kIndW; ++x) {
                       const int fx = 128 * x + 64, fy = 128 * y + 64;
                       const int ix = std::clamp((fx >> static_cast<int>(cfg.scaleS)) >> 7, 0, kIndW - 1);
                       const int iy = std::clamp((fy >> static_cast<int>(cfg.scaleT)) >> 7, 0, kIndH - 1);
                       const auto t = ind_trans(cfg, (*table)[static_cast<size_t>(iy) * kIndW + ix]);
                       ind_check_pixel(img, c, x, y, ind_wrap(cfg.wrapS, fx) + t[0], ind_wrap(cfg.wrapT, fy) + t[1]);
                     }
                   }
                 }});
  }

  // Two TEV stages: stage 0 warps by indirect texture 0 (map 2); stage 1 adds its own offset (indirect texture 1, map 3)
  // to stage 0's coordinate (add-prev, with ITW_0 so the base coordinate is not counted twice) and samples again.
  {
    static const IndCfg kFirst{.name = "addprev stage 0"};
    static const IndCfg kSecond{.name = "addprev stage 1",
                                .mtx = {{0.25f, 0.f, 0.f}, {0.f, 0.25f, 0.f}},
                                .wrapS = GX_ITW_0,
                                .wrapT = GX_ITW_0};
    auto first = std::make_shared<std::vector<IndByte>>(static_cast<size_t>(kIndW) * kIndH);
    auto second = std::make_shared<std::vector<IndByte>>(static_cast<size_t>(kIndW) * kIndH);
    for (int y = 0; y < kIndH; ++y) {
      for (int x = 0; x < kIndW; ++x) {
        const size_t i = static_cast<size_t>(y) * kIndW + x;
        (*first)[i] = ind_pick(kFirst, x, y, 0);
        const auto t0 = ind_trans(kFirst, (*first)[i]);
        (*second)[i] = ind_pick(kSecond, x, y, 1, {64 + t0[0], 64 + t0[1]});
      }
    }
    auto direct = std::make_shared<TexData>(encode_rgba8_tiled(kIndW, kIndH, ind_direct_texel));
    auto ind0 = std::make_shared<TexData>(encode_indirect(*first));
    auto ind1 = std::make_shared<TexData>(encode_indirect(*second));
    s.push_back({"tev_ind_addprev",
                 "two stages: stage 1 adds its indirect offset to stage 0's warped coordinate (add-prev, ITW_0)",
                 [direct, ind0, ind1] {
                   ind_base_state();
                   ind_load_map(0, GX_TEXMAP0, direct->data);
                   ind_load_map(1, GX_TEXMAP2, ind0->data);
                   ind_load_map(2, GX_TEXMAP3, ind1->data);
                   GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP2);
                   GXSetIndTexOrder(GX_INDTEXSTAGE1, GX_TEXCOORD1, GX_TEXMAP3);
                   GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
                   GXSetIndTexCoordScale(GX_INDTEXSTAGE1, GX_ITS_1, GX_ITS_1);
                   f32 m0[2][3], m1[2][3];
                   std::memcpy(m0, kFirst.mtx, sizeof(m0));
                   std::memcpy(m1, kSecond.mtx, sizeof(m1));
                   GXSetIndTexMtx(GX_ITM_0, m0, 0);
                   GXSetIndTexMtx(GX_ITM_1, m1, 0);
                   GXSetNumIndStages(2);
                   GXSetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF,
                                    GX_FALSE, GX_FALSE, GX_ITBA_OFF);
                   GXSetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE1, GX_ITF_8, GX_ITB_STU, GX_ITM_1, GX_ITW_0, GX_ITW_0,
                                    GX_TRUE, GX_FALSE, GX_ITBA_OFF);
                   GXSetNumTevStages(2);
                   GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                   GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                   GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
                   GXSetTevOp(GX_TEVSTAGE1, GX_REPLACE);
                   ind_quad();
                 },
                 [first, second](const Image& img, Checker& c) {
                   for (int y = 0; y < kIndH; ++y) {
                     for (int x = 0; x < kIndW; ++x) {
                       const size_t i = static_cast<size_t>(y) * kIndW + x;
                       const auto t0 = ind_trans(kFirst, (*first)[i]);
                       const auto t1 = ind_trans(kSecond, (*second)[i]);
                       ind_check_pixel(img, c, x, y, 128 * x + 64 + t0[0] + t1[0], 128 * y + 64 + t0[1] + t1[1]);
                     }
                   }
                 }});
  }
}
