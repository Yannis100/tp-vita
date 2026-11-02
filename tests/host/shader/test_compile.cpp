// Host test for gx/vita_shader.cpp: generate GLSL ES 1.00 for many TEV/lighting/fog/indirect configs and
// compile+link every program on Mesa's GLES2 (llvmpipe).
//
// What this proves: the generator emits syntactically/semantically valid GLSL ES 1.00 for every config in the
// sampled space, its uniform segment tables stay inside the uniform blob, and the stage uniform arrays /
// attribute counts are measured against the Vita (vitaGL / SGX543) limits. What it does NOT prove: that
// vitaGL's Cg-based runtime translator accepts the same source, or that the pixels are right.
//
//   tests/host/shader/run.sh [configs] [seed]
#include <gx/pipeline.hpp>
#include <gx/shader_info.hpp>
#include <gx/vita_gx.hpp>

#include "glctx.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aurora;
using namespace aurora::gx;

namespace {
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
  bool coin(uint32_t pct = 50) { return below(100) < pct; }
};

// Vita (vitaGL / SGX543 MP4) limits, see vita_shader.cpp.
constexpr uint32_t kVitaFsVec4 = 16;
constexpr uint32_t kVitaVsVec4 = 128;
constexpr uint32_t kVitaAttribs = 16;

template <typename E>
E pick(Rng& r, std::initializer_list<uint32_t> values) {
  const auto* p = values.begin();
  return static_cast<E>(p[r.below(static_cast<uint32_t>(values.size()))]);
}

void gen_attrs(Rng& r, ShaderConfig& sc, bool realistic) {
  auto set = [&](int a, uint32_t cnt, uint32_t comp) {
    AttrConfig& m = sc.attrs[a];
    m = {};
    m.attrType = GX_DIRECT;
    m.cnt = static_cast<u8>(cnt);
    m.compType = static_cast<u8>(comp);
    m.le = false;
  };
  set(GX_VA_POS, 3, GX_F32);
  if (r.coin(60)) set(GX_VA_PNMTXIDX, 1, GX_U8);
  if (r.coin(realistic ? 70 : 50)) set(GX_VA_NRM, 3, GX_F32);
  if (r.coin(70)) set(GX_VA_CLR0, 1, GX_RGBA8);
  if (r.coin(25)) set(GX_VA_CLR1, 1, GX_RGBA8);
  const uint32_t ntex = realistic ? r.below(4) : r.below(9);
  for (uint32_t t = 0; t < ntex; ++t) {
    set(GX_VA_TEX0 + t, 2, r.coin() ? GX_F32 : GX_S16);
    if (r.coin(15)) set(GX_VA_TEX0MTXIDX + t, 1, GX_U8);
  }
}

void gen_channels(Rng& r, ShaderConfig& sc) {
  for (auto& ch : sc.colorChannels) {
    ch = {};
    ch.matSrc = r.coin() ? GX_SRC_REG : GX_SRC_VTX;
    ch.ambSrc = r.coin() ? GX_SRC_REG : GX_SRC_VTX;
    ch.lightingEnabled = r.coin(30);
    if (ch.lightingEnabled) {
      ch.diffFn = pick<GXDiffuseFn>(r, {GX_DF_NONE, GX_DF_SIGN, GX_DF_CLAMP});
      ch.attnFn = pick<GXAttnFn>(r, {GX_AF_NONE, GX_AF_SPEC, GX_AF_SPOT});
    }
  }
}

void gen_tcgs(Rng& r, ShaderConfig& sc) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < sc.tevStageCount; ++i) {
    if (sc.tevStages[i].texCoordId != GX_TEXCOORD_NULL) used |= 1u << sc.tevStages[i].texCoordId;
  }
  for (uint32_t i = 0; i < sc.numIndStages; ++i) used |= 1u << sc.indStages[i].texCoordId;
  for (uint32_t c = 0; c < sc.tcgs.size(); ++c) {
    auto& t = sc.tcgs[c];
    t = {};
    if (!(used & (1u << c)) && r.coin(15)) continue; // unused coordinate
    t.type = pick<GXTexGenType>(r, {GX_TG_MTX3x4, GX_TG_MTX2x4, GX_TG_MTX2x4, GX_TG_MTX2x4, GX_TG_SRTG,
                                    GX_TG_BUMP0, GX_TG_BUMP3});
    t.src = pick<GXTexGenSrc>(r, {GX_TG_POS, GX_TG_NRM, GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2, GX_TG_TEX3, GX_TG_COLOR0,
                                  GX_TG_COLOR1, GX_TG_BINRM, GX_TG_TANGENT, GX_TG_TEXCOORD0});
    t.mtx = r.coin(40) ? GX_IDENTITY : static_cast<GXTexMtx>(GX_TEXMTX0 + 3 * r.below(10));
    t.postMtx = r.coin(60) ? GX_PTIDENTITY : static_cast<GXPTTexMtx>(GX_PTTEXMTX0 + 3 * r.below(20));
    t.normalize = r.coin(15);
    t.embossSrc = static_cast<u8>(r.below(8));
    // GX rule: an emboss-bump texgen reads an EARLIER, ordinary (non-bump) texgen as its source.
    if (t.type >= GX_TG_BUMP0 && t.type <= GX_TG_BUMP7) {
      std::vector<uint32_t> sources;
      for (uint32_t e = 0; e < c; ++e) {
        const auto& q = sc.tcgs[e];
        if (q.src != GX_MAX_TEXGENSRC && !(q.type >= GX_TG_BUMP0 && q.type <= GX_TG_BUMP7)) sources.push_back(e);
      }
      if (sources.empty()) {
        t.type = GX_TG_MTX2x4;
      } else {
        t.embossSrc = static_cast<u8>(sources[r.below(static_cast<uint32_t>(sources.size()))]);
      }
    }
  }
}

void gen_tev(Rng& r, ShaderConfig& sc, bool realistic) {
  const uint32_t n = realistic ? 1 + r.below(8) : 1 + r.below(16);
  sc.tevStageCount = n;
  for (auto& sw : sc.tevSwapTable) {
    sw.red = static_cast<GXTevColorChan>(r.below(4));
    sw.green = static_cast<GXTevColorChan>(r.below(4));
    sw.blue = static_cast<GXTevColorChan>(r.below(4));
    sw.alpha = static_cast<GXTevColorChan>(r.below(4));
  }
  sc.numIndStages = r.coin(15) ? 1 + r.below(4) : 0;
  for (uint32_t i = 0; i < sc.numIndStages; ++i) {
    sc.indStages[i].texCoordId = static_cast<GXTexCoordID>(r.below(8));
    sc.indStages[i].texMapId = static_cast<GXTexMapID>(r.below(8));
    sc.indStages[i].scaleS = static_cast<GXIndTexScale>(r.below(5));
    sc.indStages[i].scaleT = static_cast<GXIndTexScale>(r.below(5));
  }
  for (uint32_t i = 0; i < n; ++i) {
    TevStage& s = sc.tevStages[i];
    s = {};
    s.colorPass.a = static_cast<GXTevColorArg>(r.below(16));
    s.colorPass.b = static_cast<GXTevColorArg>(r.below(16));
    s.colorPass.c = static_cast<GXTevColorArg>(r.below(16));
    s.colorPass.d = static_cast<GXTevColorArg>(r.below(16));
    s.alphaPass.a = static_cast<GXTevAlphaArg>(r.below(8));
    s.alphaPass.b = static_cast<GXTevAlphaArg>(r.below(8));
    s.alphaPass.c = static_cast<GXTevAlphaArg>(r.below(8));
    s.alphaPass.d = static_cast<GXTevAlphaArg>(r.below(8));
    auto op = [&](TevOp& o) {
      o.op = r.coin(80) ? (r.coin(75) ? GX_TEV_ADD : GX_TEV_SUB) : static_cast<GXTevOp>(8 + r.below(8));
      o.bias = static_cast<GXTevBias>(r.below(3));
      o.scale = static_cast<GXTevScale>(r.below(4));
      o.outReg = static_cast<GXTevRegID>(r.below(4));
      o.clamp = r.coin(80);
    };
    op(s.colorOp);
    op(s.alphaOp);
    // Valid GX selectors only: KCSEL 0..7 and 0x0C..0x1F, KASEL 0..7 and 0x10..0x1F.
    const uint32_t kc = r.below(28);
    s.kcSel = static_cast<GXTevKColorSel>(kc < 8 ? kc : kc + 4);
    const uint32_t ka = r.below(24);
    s.kaSel = static_cast<GXTevKAlphaSel>(ka < 8 ? ka : ka + 8);
    s.texMapId = r.coin(75) ? static_cast<GXTexMapID>(r.below(8)) : GX_TEXMAP_NULL;
    // GX invariant (checked by build_shader_info): a stage that samples a texture has a texcoord.
    s.texCoordId = (s.texMapId != GX_TEXMAP_NULL || r.coin(75)) ? static_cast<GXTexCoordID>(r.below(8))
                                                                  : GX_TEXCOORD_NULL;
    s.channelId = pick<GXChannelID>(r, {GX_COLOR0A0, GX_COLOR0A0, GX_COLOR1A1, GX_COLOR_NULL, GX_ALPHA_BUMP,
                                        GX_ALPHA_BUMPN, GX_COLOR_ZERO});
    s.tevSwapRas = static_cast<GXTevSwapSel>(r.below(4));
    s.tevSwapTex = static_cast<GXTevSwapSel>(r.below(4));
    if (sc.numIndStages != 0 && r.coin(40)) {
      s.indTexStage = static_cast<GXIndTexStageID>(r.below(sc.numIndStages));
      s.indTexFormat = static_cast<GXIndTexFormat>(r.below(4));
      s.indTexBiasSel = static_cast<GXIndTexBiasSel>(r.below(8));
      s.indTexAlphaSel = static_cast<GXIndTexAlphaSel>(r.below(4));
      s.indTexMtxId = pick<GXIndTexMtxID>(r, {GX_ITM_OFF, GX_ITM_0, GX_ITM_1, GX_ITM_2, GX_ITM_S0, GX_ITM_T1});
      s.indTexWrapS = static_cast<GXIndTexWrap>(r.below(7));
      s.indTexWrapT = static_cast<GXIndTexWrap>(r.below(7));
      s.indTexUseOrigLOD = r.coin();
      s.indTexAddPrev = r.coin();
    }
  }
}

void gen_pipeline(Rng& r, PipelineConfig& pc, bool realistic) {
  pc = {};
  ShaderConfig& sc = pc.shaderConfig;
  sc = {};
  gen_attrs(r, sc, realistic);
  gen_channels(r, sc);
  gen_tev(r, sc, realistic);
  gen_tcgs(r, sc);
  sc.fogType = r.coin(70) ? GX_FOG_NONE
                          : pick<u8>(r, {GX_FOG_PERSP_LIN, GX_FOG_PERSP_EXP, GX_FOG_PERSP_EXP2, GX_FOG_PERSP_REVEXP,
                                         GX_FOG_PERSP_REVEXP2, GX_FOG_ORTHO_LIN, GX_FOG_ORTHO_EXP2});
  sc.fogRangeEnabled = sc.fogType != GX_FOG_NONE && r.coin(25);
  if (r.coin(30)) {
    sc.alphaCompare.comp0 = static_cast<GXCompare>(r.below(8));
    sc.alphaCompare.ref0 = r.below(256);
    sc.alphaCompare.op = static_cast<GXAlphaOp>(r.below(4));
    sc.alphaCompare.comp1 = static_cast<GXCompare>(r.below(8));
    sc.alphaCompare.ref1 = r.below(256);
  }
  pc.depthFunc = GX_LEQUAL;
  pc.cullMode = static_cast<GXCullMode>(r.below(4));
  pc.blendMode = static_cast<GXBlendMode>(r.below(4));
  pc.blendFacSrc = static_cast<GXBlendFactor>(r.below(8));
  pc.blendFacDst = static_cast<GXBlendFactor>(r.below(8));
  pc.blendOp = GX_LO_COPY;
  pc.dstAlpha = r.coin(70) ? UINT32_MAX : (r.coin(30) ? 0 : 1 + r.below(255));
  pc.depthCompare = r.coin(80);
  pc.depthUpdate = r.coin(70);
  pc.alphaUpdate = r.coin(70);
  pc.colorUpdate = r.coin(90);
  // GX_LINES / GX_LINESTRIP / GX_POINTS expand into screen-space quads in the vertex shader (extra attributes,
  // viewport + width uniforms); drawn last so the other fields of a config keep their draws.
  sc.lineMode = r.coin(12) ? 1 + r.below(3) : 0;
}

std::string describe(const PipelineConfig& pc) {
  const ShaderConfig& sc = pc.shaderConfig;
  char buf[256];
  std::snprintf(buf, sizeof(buf), "tev=%u ind=%u fog=%u%s alphaCmp=%d/%d blend=%d dstAlpha=%u line=%u", sc.tevStageCount,
                sc.numIndStages, sc.fogType, sc.fogRangeEnabled ? "+range" : "", sc.alphaCompare.comp0,
                sc.alphaCompare.comp1, pc.blendMode, pc.dstAlpha, static_cast<unsigned>(sc.lineMode));
  return buf;
}

// ---- fixed, TP/J3D-style configurations (Vita uniform-cap measurement) ----------------------------
struct Canon {
  std::string name;
  PipelineConfig pc;
  // True when the config is a known Vita-cap overflow that the mitigate-in-shader pass cannot fix
  // (skinned + lit + dynamic texture matrix = ~138 > 128 vertex vec4s). Such configs are pinned for
  // reporting but do not fail the canon run, exactly like the build-time warning guard.
  bool allowVsOver = false;
  // GXSetCurrentMtx value of the draw; 0 with no per-vertex matrix index lets the generator use one matrix
  // bank (pn_mtx_provably_zero). Any other value must get the full banks.
  uint32_t currentPnMtx = 0;
  // Light slots the program reads (vita::light_bucket); the replay derives it from the draw's light masks.
  uint32_t lights = vita::MaxLightSlots;
};

AttrConfig direct(uint32_t cnt, uint32_t comp) {
  AttrConfig m{};
  m.attrType = GX_DIRECT;
  m.cnt = static_cast<u8>(cnt);
  m.compType = static_cast<u8>(comp);
  m.le = false;
  return m;
}

PipelineConfig canon_base() {
  PipelineConfig pc{};
  pc.depthFunc = GX_LEQUAL;
  pc.cullMode = GX_CULL_BACK;
  pc.blendMode = GX_BM_NONE;
  pc.blendFacSrc = GX_BL_SRCALPHA;
  pc.blendFacDst = GX_BL_INVSRCALPHA;
  pc.blendOp = GX_LO_COPY;
  pc.dstAlpha = UINT32_MAX;
  pc.depthCompare = pc.depthUpdate = pc.alphaUpdate = pc.colorUpdate = true;
  pc.shaderConfig.attrs[GX_VA_POS] = direct(3, GX_F32);
  return pc;
}

void tev_tex_times_ras(ShaderConfig& sc, uint32_t i, uint32_t map, uint32_t coord) {
  TevStage& s = sc.tevStages[i];
  s = {};
  s.colorPass.b = GX_CC_TEXC;
  s.colorPass.c = GX_CC_RASC;
  s.alphaPass.b = GX_CA_TEXA;
  s.alphaPass.c = GX_CA_RASA;
  s.texMapId = static_cast<GXTexMapID>(map);
  s.texCoordId = static_cast<GXTexCoordID>(coord);
  s.channelId = GX_COLOR0A0;
  sc.tevStageCount = std::max<uint32_t>(sc.tevStageCount, i + 1);
}

void tev_prev_plus_konst(ShaderConfig& sc, uint32_t i, GXTevKColorSel kc) {
  TevStage& s = sc.tevStages[i];
  s = {};
  s.colorPass.a = GX_CC_CPREV;
  s.colorPass.b = GX_CC_KONST;
  s.colorPass.c = GX_CC_APREV;
  s.alphaPass.d = GX_CA_APREV;
  s.kcSel = kc;
  sc.tevStageCount = std::max<uint32_t>(sc.tevStageCount, i + 1);
}

void tcg_mtx(ShaderConfig& sc, uint32_t c, GXTexGenSrc src, GXTexMtx mtx = GX_IDENTITY) {
  sc.tcgs[c] = {};
  sc.tcgs[c].type = GX_TG_MTX2x4;
  sc.tcgs[c].src = src;
  sc.tcgs[c].mtx = mtx;
  sc.tcgs[c].postMtx = GX_PTIDENTITY;
}

void make_lit(ShaderConfig& sc) {
  sc.attrs[GX_VA_NRM] = direct(3, GX_F32);
  sc.colorChannels[GX_COLOR0].lightingEnabled = true;
  sc.colorChannels[GX_COLOR0].diffFn = GX_DF_CLAMP;
  sc.colorChannels[GX_COLOR0].attnFn = GX_AF_SPOT;
  sc.colorChannels[GX_COLOR0].matSrc = GX_SRC_REG;
  sc.colorChannels[GX_COLOR0].ambSrc = GX_SRC_REG;
  sc.colorChannels[GX_ALPHA0].matSrc = GX_SRC_REG;
}

std::vector<Canon> make_canon() {
  std::vector<Canon> v;
  {
    Canon c{"A  unlit vertex color (solid/UI)", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8);
    sc.tevStages[0] = {};
    sc.tevStages[0].colorPass.d = GX_CC_RASC;
    sc.tevStages[0].alphaPass.d = GX_CA_RASA;
    sc.tevStages[0].channelId = GX_COLOR0A0;
    sc.tevStageCount = 1;
    v.push_back(c);
  }
  {
    Canon c{"B  unlit textured x vertex color, 1 stage", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8);
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    tcg_mtx(sc, 0, GX_TG_TEX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
  }
  {
    Canon c{"C  B + alpha test (cutout)", v.back().pc};
    c.pc.shaderConfig.alphaCompare.comp0 = GX_GREATER;
    c.pc.shaderConfig.alphaCompare.ref0 = 128;
    c.pc.shaderConfig.alphaCompare.op = GX_AOP_AND;
    v.push_back(c);
  }
  {
    Canon c{"D  B + static texture matrix (GX_TEXMTX0)", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8);
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    tcg_mtx(sc, 0, GX_TG_TEX0, GX_TEXMTX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
  }
  {
    Canon c{"E  skinned (PNMTXIDX), 1 light-lit, 1 tex, 2 stages + KColor", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_PNMTXIDX] = direct(1, GX_U8);
    sc.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8);
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    make_lit(sc);
    tcg_mtx(sc, 0, GX_TG_TEX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    tev_prev_plus_konst(sc, 1, GX_TEV_KCSEL_K0);
    v.push_back(c);
  }
  {
    Canon c{"F  E with linear fog", v.back().pc};
    c.pc.shaderConfig.fogType = GX_FOG_PERSP_LIN;
    v.push_back(c);
  }
  {
    Canon c{"G  unskinned lit, 1 tex, 1 stage (static geometry)", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    make_lit(sc);
    tcg_mtx(sc, 0, GX_TG_TEX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
  }
  {
    Canon c{"I  unskinned lit + dyn texmtx, current matrix 0 (one matrix bank)", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    sc.attrs[GX_VA_TEX0MTXIDX] = direct(1, GX_U8);
    make_lit(sc);
    tcg_mtx(sc, 0, GX_TG_TEX0, GX_TEXMTX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
  }
  {
    // Same shader config as I, but the game called GXSetCurrentMtx(GX_PNMTX1) (d_home_button.cpp does).
    // aPnMtxIdx is then the constant 1, so every bank must be uploaded: no trimming possible.
    Canon c{"I2 unskinned lit + dyn texmtx, current matrix 1 (full banks, residual overflow)", canon_base()};
    c.allowVsOver = true;
    c.currentPnMtx = 1;
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    sc.attrs[GX_VA_TEX0MTXIDX] = direct(1, GX_U8);
    make_lit(sc);
    tcg_mtx(sc, 0, GX_TG_TEX0, GX_TEXMTX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
  }
  {
    Canon c{"J  skinned lit + dynamic texture matrix (worst case, residual overflow)", canon_base()};
    c.allowVsOver = true;
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_PNMTXIDX] = direct(1, GX_U8);
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    sc.attrs[GX_VA_TEX0MTXIDX] = direct(1, GX_U8);
    make_lit(sc);
    tcg_mtx(sc, 0, GX_TG_TEX0, GX_TEXMTX0);
    tev_tex_times_ras(sc, 0, 0, 0);
    v.push_back(c);
    // Same draw, but its masks only enable the first few lights: the replay then builds a smaller program.
    // 6 slots is exactly the vitaGL cap, so up to 6 enabled lights on the worst-case material fit; 7-8 do not.
    for (const uint32_t n : {2u, 4u, 6u}) {
      Canon d = c;
      d.name = "J" + std::to_string(n) + " J with " + std::to_string(n) + " light slots (lights enabled up to " +
               std::to_string(n) + ")";
      d.lights = n;
      d.allowVsOver = false;
      v.push_back(d);
    }
  }
  {
    Canon c{"H  effect: 2 tex, 4 stages, TEVREG0/KColors, fog, vertex color", canon_base()};
    ShaderConfig& sc = c.pc.shaderConfig;
    sc.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8);
    sc.attrs[GX_VA_TEX0] = direct(2, GX_F32);
    sc.attrs[GX_VA_TEX1] = direct(2, GX_F32);
    tcg_mtx(sc, 0, GX_TG_TEX0, GX_TEXMTX0);
    tcg_mtx(sc, 1, GX_TG_TEX1, GX_TEXMTX1);
    tev_tex_times_ras(sc, 0, 0, 0);
    tev_tex_times_ras(sc, 1, 1, 1);
    tev_prev_plus_konst(sc, 2, GX_TEV_KCSEL_K1);
    tev_prev_plus_konst(sc, 3, GX_TEV_KCSEL_K2_R);
    sc.tevStages[3].colorPass.d = GX_CC_C0; // loads TEVREG0
    sc.fogType = GX_FOG_PERSP_EXP2;
    c.pc.blendMode = GX_BM_BLEND;
    v.push_back(c);
  }
  return v;
}

int run_canon() {
  const auto list = make_canon();
  std::printf("%-62s %5s %5s %5s  %s\n", "config", "vsV4", "fsV4", "attr", "Mesa link / Vita caps (vs<=128, fs<=16)");
  int bad = 0;
  for (const auto& c : list) {
    const ShaderInfo info = build_shader_info(c.pc.shaderConfig);
    const bool pnZero = vita::pn_mtx_provably_zero(c.pc.shaderConfig, c.currentPnMtx);
    const vita::ProgramSource ps = vita::build_program_source(c.pc, info, pnZero, c.lights);
    std::vector<std::string> names;
    for (uint32_t a = 0; a < ps.layout.count; ++a) names.emplace_back(ps.layout.attrs[a].name);
    std::string log;
    const GLuint prog = hostgl::link(ps.vertex, ps.fragment, names, log);
    if (prog) glDeleteProgram(prog);
    const bool vsOk = ps.vsVec4 <= kVitaVsVec4;
    const bool fsOk = ps.fsVec4 <= kVitaFsVec4;
    bad += !prog || (!vsOk && !c.allowVsOver) || !fsOk; // a known overflow (allowVsOver) is reported, not failed
    const char* vsTag = vsOk ? "ok" : (c.allowVsOver ? "OVER(j)" : "OVER");
    std::printf("%-62s %5u %5u %5u  %s | vs %s fs %s\n", c.name.c_str(), ps.vsVec4, ps.fsVec4, ps.layout.count,
                prog ? "links" : ("LINK FAIL: " + log.substr(0, 120)).c_str(), vsTag, fsOk ? "ok" : "OVER");
  }
  return bad;
}
// ---- fog range table: encoding, then the real GLSL decoder on a real texture/sampler -------------------
// build_fog_range_lut (command_processor.cpp) produces sqrt(offset^2 + k^2) / k, always >= 1. The first
// replay implementation clamped to [0,1] into 8 bits, which turned every texel into exactly 1.0 (fog
// range silently disabled); the checks below fail for that scheme.
std::vector<float> make_fog_lut(uint32_t width, float rangeCenter, const std::array<int, 10>& rangeKRaw) {
  std::array<float, 10> rangeK;
  for (uint32_t i = 0; i < rangeK.size(); ++i) {
    const uint32_t source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<float>(rangeKRaw[source]) / 64.f;
  }
  std::vector<float> lut(width);
  for (uint32_t x = 0; x < width; ++x) {
    const float screenX = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width)) * 2.f - 1.f;
    const float offset = screenX - rangeCenter;
    const float rangeIndex = std::clamp(9.f - std::fabs(offset) * 9.f, 0.f, 9.f);
    const auto lower = static_cast<uint32_t>(rangeIndex);
    const uint32_t upper = std::min(lower + 1, 9u);
    const float fraction = rangeIndex - static_cast<float>(lower);
    const float k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

int run_fog_range() {
  int bad = 0;
  // 1. CPU round trip: within one LSB of the 16-bit scale for [0, FogRangeMax], saturating above.
  const float lsb = vita::FogRangeMax / 65535.f;
  for (float v = 0.f; v <= vita::FogRangeMax; v += 0.0137f) {
    uint8_t hi, lo;
    vita::fog_range_encode(v, hi, lo);
    bad += std::fabs(vita::fog_range_decode(hi, lo) - v) > lsb;
  }
  uint8_t hi, lo;
  vita::fog_range_encode(1e9f, hi, lo);
  bad += vita::fog_range_decode(hi, lo) != vita::FogRangeMax;
  vita::fog_range_encode(std::nanf(""), hi, lo);
  bad += vita::fog_range_decode(hi, lo) != 0.f;
  vita::fog_range_encode(-3.f, hi, lo);
  bad += vita::fog_range_decode(hi, lo) != 0.f;

  // 2. Real-looking tables: the data must really exceed 1 (otherwise this test proves nothing).
  struct Case {
    float center;
    std::array<int, 10> k;
  };
  const Case cases[] = {
      {0.0f, {30, 30, 30, 30, 30, 30, 30, 30, 30, 30}},
      {-0.25f, {4, 8, 16, 24, 32, 40, 48, 56, 64, 72}},
      {0.4f, {200, 160, 120, 90, 60, 40, 24, 12, 6, 3}},
  };
  constexpr uint32_t W = 640;
  for (const Case& c : cases) {
    const std::vector<float> lut = make_fog_lut(W, c.center, c.k);
    const float maxFactor = *std::max_element(lut.begin(), lut.end());
    const float minFactor = *std::min_element(lut.begin(), lut.end());
    if (!(minFactor >= 1.f && maxFactor > 1.5f)) {
      std::printf("fog LUT sanity: min %f max %f (expected >= 1 and a real spread)\n", minFactor, maxFactor);
      ++bad;
    }
    // Old scheme (clamp to [0,1], 8 bit): every texel 255 -> factor 1. Show that it destroys the table.
    uint32_t distinctOld = 0;
    {
      std::set<int> seen;
      for (float f : lut) seen.insert(static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f));
      distinctOld = static_cast<uint32_t>(seen.size());
    }
    if (distinctOld != 1) {
      std::printf("negative control: old scheme should collapse to one value, got %u\n", distinctOld);
      ++bad;
    }

    // 3. GPU: upload with the replay's encoding, decode with the generator's GLSL helper at gl_FragCoord.x,
    // write the 16-bit result back out as RGBA8 (r = high byte, g = low byte) and compare with the CPU value.
    std::vector<uint8_t> rgba(W * 4);
    for (uint32_t x = 0; x < W; ++x) {
      vita::fog_range_encode(lut[x], rgba[x * 4 + 0], rgba[x * 4 + 1]);
      rgba[x * 4 + 2] = 0;
      rgba[x * 4 + 3] = 255;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const std::string vs = "attribute vec2 aPos;\nvoid main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const std::string fs = std::string("precision highp float;\nuniform sampler2D uFogRange;\nuniform vec2 uFogRangeSize;\n") +
                           vita::FogRangeDecodeGlsl +
                           "void main() {\n"
                           "  float s = " + std::string(vita::FogRangeSampleGlsl) + ";\n"
                           "  float n = floor(s * (65535.0 / 32.0) + 0.5);\n"
                           "  float hi = floor(n / 256.0);\n"
                           "  gl_FragColor = vec4(hi / 255.0, (n - hi * 256.0) / 255.0, 0.0, 1.0);\n"
                           "}\n";
    std::string log;
    const GLuint prog = hostgl::link(vs, fs, {"aPos"}, log);
    if (prog == 0) {
      std::printf("fog decode shader failed: %s\n", log.c_str());
      return 1;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uFogRange"), 0);
    glUniform2f(glGetUniformLocation(prog, "uFogRangeSize"), 1.f / static_cast<float>(W), 0.f);
    glViewport(0, 0, W, 480);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    const float tri[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const std::vector<uint8_t> px = hostgl::read_rgba(W, 480);
    uint32_t mismatches = 0;
    for (uint32_t x = 0; x < W; ++x) {
      const uint8_t* p = &px[(240u * W + x) * 4]; // any row: the table has no Y dependence
      const uint32_t gpu = p[0] * 256u + p[1];
      const uint32_t cpu = rgba[x * 4] * 256u + rgba[x * 4 + 1];
      mismatches += gpu != cpu;
    }
    std::printf("fog range case centre %+.2f: factor %.3f..%.3f, GPU decode mismatches %u / %u\n", c.center, minFactor,
                maxFactor, mismatches, W);
    bad += mismatches != 0;
    glDeleteProgram(prog);
    glDeleteTextures(1, &tex);
  }
  std::printf("fog range: %s\n", bad == 0 ? "ok" : "FAILED");
  return bad;
}
} // namespace

// convert_int_lanes: u32 bit fields become equal-valued floats, other lanes and other vec4s are untouched.
static int run_int_lanes() {
  int bad = 0;
  const auto bits = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
  float v[8] = {1.5f, bits(5), -2.f, bits(255), 3.5f, bits(9), 7.25f, bits(128)};
  float keep[8];
  std::memcpy(keep, v, sizeof(v));
  vita::convert_int_lanes(v, 2, 0x0);
  if (std::memcmp(keep, v, sizeof(v)) != 0) { std::printf("FAIL int lanes: mask 0 changed data\n"); ++bad; }
  vita::convert_int_lanes(v, 2, 0xA); // lanes 1 and 3 of both vec4s
  const float want[8] = {1.5f, 5.f, -2.f, 255.f, 3.5f, 9.f, 7.25f, 128.f};
  for (int i = 0; i < 8; ++i) {
    if (std::memcmp(&v[i], &want[i], 4) != 0) { std::printf("FAIL int lanes: lane %d = %g, want %g\n", i, v[i], want[i]); ++bad; }
  }
  // A raw small integer read as float is a denormal: the failure mode this conversion removes.
  if (!(bits(5) < 1e-30f)) { std::printf("FAIL int lanes: premise (u32 5 reads as ~0) does not hold\n"); ++bad; }
  std::printf("convert_int_lanes unit test: %s\n", bad == 0 ? "ok" : "FAILED");
  return bad;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::set_terminate([] {
    try {
      if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      std::printf("terminate: uncaught exception: %s\n", ex.what());
    } catch (...) {
      std::printf("terminate: uncaught non-std exception\n");
    }
    std::abort();
  });
  if (argc > 1 && std::string(argv[1]) == "fog") {
    if (!hostgl::init()) return 2;
    const int bad = run_fog_range();
    hostgl::shutdown();
    return bad != 0;
  }
  if (argc > 1 && std::string(argv[1]) == "canon") {
    if (!hostgl::init()) return 2;
    const int bad = run_canon();
    hostgl::shutdown();
    return bad != 0;
  }
  const uint32_t count = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 2000;
  const uint64_t seed = argc > 2 ? static_cast<uint64_t>(std::atoll(argv[2])) : 1;
  const long dumpIndex = argc > 3 ? std::atol(argv[3]) : -1; // print the sources of this config index
  if (!hostgl::init()) {
    std::printf("no GLES2 context\n");
    return 2;
  }
  Rng rng(seed);
  uint32_t ok = 0, fail = 0, segBad = 0, fsOver = 0, vsOver = 0, attrOver = 0;
  uint32_t trimBad = 0, trimmed = 0, trimNonZeroFull = 0;
  uint32_t intBad = 0, lightMaskDraws = 0, lineDraws = 0;
  uint32_t slotBad = 0, slotLinkFail = 0, slotLit = 0, slotSaved = 0, maskBad = 0;
  uint32_t realisticOk = 0, realisticTotal = 0, realisticFsOver = 0;
  uint32_t maxFs = 0, maxVs = 0, maxAttrs = 0;
  std::map<uint32_t, uint32_t> fsHist;
  std::set<std::string> seenErrors;
  std::vector<std::string> failExamples;

  for (uint32_t i = 0; i < count; ++i) {
    const bool realistic = (i % 2) == 0;
    PipelineConfig pc;
    gen_pipeline(rng, pc, realistic);
    const ShaderInfo info = build_shader_info(pc.shaderConfig);
    // Draws use GXSetCurrentMtx 0 most of the time, but the game also uses PNMTX1 (value 1) and others.
    const uint32_t currentPnMtx = rng.coin(60) ? 0 : rng.below(MaxPnMtx);
    const bool pnZero = vita::pn_mtx_provably_zero(pc.shaderConfig, currentPnMtx);
    const vita::ProgramSource ps = vita::build_program_source(pc, info, pnZero);
    {
      // Trim contract: the helper only allows it for no-matrix-index formats with current matrix 0; the
      // untrimmed program differs by exactly the 9 dropped banks of pnPos (27 vec4) and, if the shader
      // reads normals, of pnNrm (another 27). A trimmed program must never be used for another matrix.
      const bool noIdx = pc.shaderConfig.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE;
      if (pnZero != (noIdx && currentPnMtx == 0)) ++trimBad;
      const vita::ProgramSource full = vita::build_program_source(pc, info, false);
      const int delta = static_cast<int>(full.vsVec4) - static_cast<int>(ps.vsVec4);
      if (pnZero ? (delta != 27 && delta != 54) : delta != 0) ++trimBad;
      // Forcing the flag on for a skinned format must be ignored (the index can select any bank).
      if (!noIdx && vita::build_program_source(pc, info, true).vsVec4 != full.vsVec4) ++trimBad;
      if (noIdx && currentPnMtx != 0) ++trimNonZeroFull;
      if (pnZero) ++trimmed;
    }
    {
      // u32 lanes of the blob (light masks, point/line texcoord mask) must be flagged for conversion at upload:
      // exactly one all-lane segment when the vertex shader tests light bits, exactly one .w segment for line /
      // point expansion, and none anywhere else (every other lane is a float).
      uint32_t lightSegs = 0, lineSegs = 0, otherInt = 0;
      for (const auto& seg : ps.vsSegs) {
        if (seg.intLanes == 0xF && seg.count == 1) ++lightSegs;
        else if (seg.intLanes == 0x8 && seg.count == 1) ++lineSegs;
        else if (seg.intLanes != 0) ++otherInt;
      }
      for (const auto& seg : ps.fsSegs) otherInt += seg.intLanes != 0;
      const bool usesLightMask = ps.vertex.find("float lmask = ") != std::string::npos;
      if (lightSegs != (usesLightMask ? 1u : 0u)) ++intBad;
      if (lineSegs != (pc.shaderConfig.lineMode != 0 ? 1u : 0u)) ++intBad;
      if (otherInt != 0) ++intBad;
      lightMaskDraws += usesLightMask;
      lineDraws += pc.shaderConfig.lineMode != 0;
    }
    {
      // Light slots. A draw's masks pick the slot count (lights_in_use); the generator must then carry exactly
      // those slots for a plainly lit shader, never more than asked for, and nothing for an unlit one.
      const uint32_t buckets[5] = {1, 2, 4, 6, 8};
      const uint32_t want = buckets[rng.below(5)];
      const vita::ProgramSource few = vita::build_program_source(pc, info, pnZero, want);
      const int delta = static_cast<int>(ps.vsVec4) - static_cast<int>(few.vsVec4);
      bool chanLit = false, emboss = false;
      for (uint32_t k = 0; k < info.sampledColorChannels.size(); ++k) {
        if (info.sampledColorChannels.test(k) &&
            (pc.shaderConfig.colorChannels[k].lightingEnabled || pc.shaderConfig.colorChannels[k + GX_ALPHA0].lightingEnabled)) {
          chanLit = true;
        }
      }
      for (uint32_t k = 0; k < MaxTexCoord; ++k) {
        const auto type = pc.shaderConfig.tcgs[k].type;
        if (info.sampledTexCoords.test(k) && type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7) emboss = true;
      }
      const int full = static_cast<int>(8 - want) * 5;
      if (delta < 0 || delta % 5 != 0) ++slotBad;
      else if (!chanLit && delta != 0) ++slotBad;
      else if (chanLit && !emboss && delta != full) ++slotBad;
      else if (chanLit && emboss && delta > full) ++slotBad;
      if (chanLit) ++slotLit;
      if (delta > 0) ++slotSaved;
      std::vector<std::string> fnames;
      for (uint32_t a = 0; a < few.layout.count; ++a) fnames.emplace_back(few.layout.attrs[a].name);
      std::string flog;
      const GLuint fprog = hostgl::link(few.vertex, few.fragment, fnames, flog);
      if (fprog != 0) glDeleteProgram(fprog); else ++slotLinkFail;

      // lights_in_use against random masks: lanes of unlit channels are ignored, and the bucket always covers the
      // highest light a lit channel enables (a draw must never lose a light).
      const vita::LightMaskLocation loc = vita::light_mask_location(pc.shaderConfig, info);
      std::vector<uint8_t> blob(info.uniformSize, 0);
      uint32_t expectHighest = 0, laneVals[4] = {0, 0, 0, 0};
      for (uint32_t k = 0; k < 4; ++k) {
        laneVals[k] = rng.coin(40) ? 0u : rng.below(1u << 8);
        if (rng.coin(10)) laneVals[k] |= 0xFFFFFF00u; // junk above the 8 light bits must be ignored
      }
      bool litLane[4] = {false, false, false, false};
      for (uint32_t k = 0; k < info.sampledColorChannels.size(); ++k) {
        if (!info.sampledColorChannels.test(k)) continue;
        litLane[k] = pc.shaderConfig.colorChannels[k].lightingEnabled;
        litLane[k + GX_ALPHA0] = pc.shaderConfig.colorChannels[k + GX_ALPHA0].lightingEnabled;
      }
      for (uint32_t k = 0; k < 4; ++k) {
        if (litLane[k]) {
          for (uint32_t bit = 0; bit < 8; ++bit) {
            if ((laneVals[k] >> bit) & 1u) expectHighest = std::max(expectHighest, bit + 1);
          }
        }
      }
      if (loc.vec4 >= 0) {
        if (static_cast<size_t>(loc.vec4 + 1) * 16 > blob.size()) ++maskBad;
        else for (uint32_t k = 0; k < 4; ++k) std::memcpy(&blob[static_cast<size_t>(loc.vec4) * 16 + k * 4], &laneVals[k], 4);
      }
      const uint32_t got = vita::lights_in_use(loc, blob.data(), blob.size());
      const bool anyLit = litLane[0] || litLane[1] || litLane[2] || litLane[3];
      const uint32_t expect = anyLit ? vita::light_bucket(expectHighest) : vita::MaxLightSlots;
      if (got != expect || (anyLit && got < expectHighest)) ++maskBad;
      if (anyLit != chanLit && info.lightingEnabled) ++maskBad; // litChannels must agree with the generator's view
    }
    const uint32_t blobVec4 = info.uniformSize / 16;
    for (const auto& seg : ps.vsSegs) segBad += (seg.src + seg.count > blobVec4 || seg.dst + seg.count > ps.vsVec4);
    for (const auto& seg : ps.fsSegs) segBad += (seg.src + seg.count > blobVec4 || seg.dst + seg.count > ps.fsVec4);

    if (static_cast<long>(i) == dumpIndex) {
      std::printf("// cfg#%u [%s]\n// ---- VERTEX (vsVec4=%u) ----\n%s\n// ---- FRAGMENT (fsVec4=%u) ----\n%s\n", i,
                  describe(pc).c_str(), ps.vsVec4, ps.vertex.c_str(), ps.fsVec4, ps.fragment.c_str());
      return 0;
    }
    std::vector<std::string> names;
    for (uint32_t a = 0; a < ps.layout.count; ++a) names.emplace_back(ps.layout.attrs[a].name);
    std::string log;
    const GLuint prog = hostgl::link(ps.vertex, ps.fragment, names, log);

    maxFs = std::max<uint32_t>(maxFs, ps.fsVec4);
    maxVs = std::max<uint32_t>(maxVs, ps.vsVec4);
    maxAttrs = std::max<uint32_t>(maxAttrs, ps.layout.count);
    fsHist[ps.fsVec4]++;
    const bool over = ps.fsVec4 > kVitaFsVec4;
    fsOver += over;
    vsOver += ps.vsVec4 > kVitaVsVec4;
    attrOver += ps.layout.count > kVitaAttribs;
    if (realistic) {
      ++realisticTotal;
      realisticFsOver += over;
    }
    if (prog != 0) {
      ++ok;
      realisticOk += realistic;
      glDeleteProgram(prog);
    } else {
      ++fail;
      // Group by the first error line so a systematic bug shows up once.
      const std::string firstLine = log.substr(0, log.find('\n'));
      if (seenErrors.insert(firstLine).second && failExamples.size() < 6) {
        failExamples.push_back("cfg#" + std::to_string(i) + " [" + describe(pc) + "]\n  " + log.substr(0, 600));
      }
    }
  }

  std::printf("configs: %u  linked on Mesa: %u  FAILED: %u  (distinct first errors: %zu)\n", count, ok, fail,
              seenErrors.size());
  std::printf("realistic subset (<=8 TEV stages, <=3 tex): %u/%u linked, %u need >%u fragment vec4 (Vita cap)\n",
              realisticOk, realisticTotal, realisticFsOver, kVitaFsVec4);
  std::printf("segment-table violations: %u\n", segBad);
  std::printf("u32-lane flags: %u configs with light masks, %u line/point configs, contract violations: %u\n",
              lightMaskDraws, lineDraws, intBad);
  std::printf("light slots: %u lit configs, %u programs shrunk, slot-count violations: %u, link failures: %u, "
              "light-mask lookup violations: %u\n", slotLit, slotSaved, slotBad, slotLinkFail, maskBad);
  std::printf("matrix-bank trim: %u configs trimmed, %u unskinned draws with current matrix != 0 kept full banks, "
              "contract violations: %u\n", trimmed, trimNonZeroFull, trimBad);
  std::printf("max fsVec4=%u maxVsVec4=%u maxAttribs=%u | over Vita caps: fs>%u: %u, vs>%u: %u, attribs>%u: %u\n", maxFs,
              maxVs, maxAttrs, kVitaFsVec4, fsOver, kVitaVsVec4, vsOver, kVitaAttribs, attrOver);
  std::printf("fsVec4 histogram:");
  for (const auto& [k, v] : fsHist) std::printf(" %u:%u", k, v);
  std::printf("\n");
  for (const auto& e : failExamples) std::printf("---- first failures ----\n%s\n", e.c_str());
  hostgl::shutdown();
  return (fail != 0 || segBad != 0 || trimBad != 0 || intBad != 0 || slotBad != 0 || slotLinkFail != 0 || maskBad != 0 ||
          run_int_lanes() != 0)
             ? 1
             : 0;
}
