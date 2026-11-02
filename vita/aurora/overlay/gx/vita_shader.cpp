// Vita backend: GX state -> GLSL ES 1.00 program source (see vita_gx.hpp for the contract).
//
// A port of aurora/lib/gx/shader.cpp (the WGSL generator): same TEV / texgen / lighting / fog /
// indirect / emboss semantics, but
//   * vertices arrive pre-decoded as GLSL attributes (no storage-buffer vertex pulling),
//   * the packed uniform blob (gx::build_uniform) is split into per-stage vec4 arrays uVS[] / uFS[]
//     (UniformSeg maps blob ranges to array positions), so each stage only receives what it reads,
//   * integer/bit tricks are done in float (GLSL ES 1.00 has neither),
//   * the clip position is converted from Aurora's WebGPU convention (z in [0,w], y up, row 0 on
//     top) to GL: gl_Position = (x, -y, 2z - w, w). Y is flipped so FBO row 0 is the GX top row.
//
// Known gaps compared to upstream (also listed in the final report):
//   * LOD bias of texture objects is ignored (kUseTexLodBias),
//   * alpha-channel TEV compare ops (A8/GR16/BGR24/RGB8) all compare the single alpha value.
#include "vita_gx.hpp"

#include "shader_info.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::gx::vita {
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {
constexpr Module Log{"aurora::gx::vita"};

// Fragment float precision. TEV math is defined in 8..10 bit fixed point, GXM half precision would
// show banding and break the byte-space compare ops; switch to "mediump" only after measuring.
constexpr std::string_view kFragmentPrecision = "highp"sv;
// texture2D(s, uv, bias) in fragment shaders is plain ES 1.00, but vitaGL's GLSL translator is not
// known to handle it, and GL ES has no per-texture LOD bias. Off until proven.
constexpr bool kUseTexLodBias = false;
// vitaGL: 8 varying vec4s.
constexpr int kMaxVaryingVec4 = 8;

// vitaGL / SGX543 MP4 uniform limits (GL_MAX_VERTEX_UNIFORM_VECTORS /
// GL_MAX_FRAGMENT_UNIFORM_VECTORS as reported by vitaGL). The fragment limit is the tight one: a
// worst-case TEV shader (4 TEV regs + 4 KColors + fog + texcoord scales + 3 indirect matrices +
// 8 texture sizes + a scratch uniform) needs ~31 vec4s and would exceed 16 and fail to link.
// Individual draws stay far below that; build() warns when the per-stage array would blow each cap.
constexpr uint32_t kMaxVertexUniformVec4 = 128;
constexpr uint32_t kMaxFragmentUniformVec4 = 16;

constexpr uint32_t kLightVec4 = 5; // sizeof(Light) / 16
constexpr uint32_t kFogVec4 = 5;   // sizeof(Fog) / 16

// ---- helpers shared with upstream's generator --------------------------------------------------
std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "r";
  }
}

bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }
constexpr bool is_emboss_texgen(GXTexGenType type) noexcept { return type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7; }

bool uses_texture_sample(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

std::string_view tev_bias(GXTevBias bias) {
  switch (bias) {
  case GX_TB_ADDHALF:
    return " + 0.5"sv;
  case GX_TB_SUBHALF:
    return " - 0.5"sv;
  case GX_TB_ZERO:
  default:
    return ""sv;
  }
}

std::string_view tev_scale(GXTevScale scale) {
  switch (scale) {
  case GX_CS_SCALE_2:
    return " * 2.0"sv;
  case GX_CS_SCALE_4:
    return " * 4.0"sv;
  case GX_CS_DIVIDE_2:
    return " / 2.0"sv;
  case GX_CS_SCALE_1:
  default:
    return ""sv;
  }
}

// ---- alpha compare as a small boolean expression tree (folds constants like upstream) ----------
struct AlphaCompareExpr {
  std::string expr;
  int constant = -1; // 0 / 1 when the value is known at generation time
};

AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& e) {
  if (e.constant != -1) {
    return alpha_compare_const(e.constant == 0);
  }
  return {fmt::format("!{}", e.expr), -1};
}

AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& l, const AlphaCompareExpr& r) {
  if (l.constant == 0 || r.constant == 0) {
    return alpha_compare_const(false);
  }
  if (l.constant == 1) {
    return r;
  }
  if (r.constant == 1) {
    return l;
  }
  return {fmt::format("({} && {})", l.expr, r.expr), -1};
}

AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& l, const AlphaCompareExpr& r) {
  if (l.constant == 1 || r.constant == 1) {
    return alpha_compare_const(true);
  }
  if (l.constant == 0) {
    return r;
  }
  if (r.constant == 0) {
    return l;
  }
  return {fmt::format("({} || {})", l.expr, r.expr), -1};
}

AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& l, const AlphaCompareExpr& r) {
  if (l.constant != -1 && r.constant != -1) {
    return alpha_compare_const(l.constant != r.constant);
  }
  if (l.constant == 0) {
    return r;
  }
  if (r.constant == 0) {
    return l;
  }
  if (l.constant == 1) {
    return alpha_compare_not(r);
  }
  if (r.constant == 1) {
    return alpha_compare_not(l);
  }
  return {fmt::format("({} != {})", l.expr, r.expr), -1};
}

AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& l, const AlphaCompareExpr& r) {
  if (l.constant != -1 && r.constant != -1) {
    return alpha_compare_const(l.constant == r.constant);
  }
  if (l.constant == 0) {
    return alpha_compare_not(r);
  }
  if (r.constant == 0) {
    return alpha_compare_not(l);
  }
  if (l.constant == 1) {
    return r;
  }
  if (r.constant == 1) {
    return l;
  }
  return {fmt::format("({} == {})", l.expr, r.expr), -1};
}

// `aref` is the 0..255 float alpha computed in main().
AlphaCompareExpr alpha_compare(GXCompare comp, u32 ref) {
  const auto iref = static_cast<u32>(static_cast<u8>(ref));
  switch (comp) {
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (iref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(aref < {}.0)", iref), -1};
  case GX_LEQUAL:
    if (iref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(aref <= {}.0)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(aref == {}.0)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(aref != {}.0)", iref), -1};
  case GX_GEQUAL:
    if (iref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(aref >= {}.0)", iref), -1};
  case GX_GREATER:
    if (iref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(aref > {}.0)", iref), -1};
  case GX_ALWAYS:
  default:
    return alpha_compare_const(true);
  }
}

// ---- uniform blob layout (must mirror build_shader_info / fill_uniform in shader_info.cpp) -----
// All offsets are in vec4 units from the start of the blob.
struct BlobLayout {
  uint16_t viewport = 0; // render viewport (xy), logical viewport (zw)
  int lineParams = -1;   // line_width, line_aspect_y, line_tex_offset, line_texcoord_mask
  uint16_t proj = 0;     // 4 rows
  uint16_t pnPos = 0;    // MaxPnMtx x 3 rows
  uint16_t texMtx = 0;   // MaxTexMtx x 3 rows
  uint16_t pnNrm = 0;    // MaxPnMtx x 3 rows
  std::array<int, MaxTevRegs> tevReg{-1, -1, -1, -1};
  int lights = -1;     // MaxLights x kLightVec4
  int lightState = -1; // 4 lanes: lightMask of COLOR0, COLOR1, ALPHA0, ALPHA1
  struct Chan {
    int amb = -1, mat = -1, ambA = -1, matA = -1;
  };
  std::array<Chan, MaxColorChannels / 2> chan{};
  std::array<int, MaxKColors> kcolor{-1, -1, -1, -1};
  int ptTexMtx = -1; // MaxPTTexMtx x 3 rows
  int fog = -1;      // kFogVec4: color, (a,b,c,rangeCenter), rangeK[3]
  uint16_t texcoordScale = 0; // MaxTexCoord, (scaleS+1, scaleT+1, 0, 0)
  int indMtx = -1;            // MaxIndTexMtxs x 2: (m0.xy, m1.xy), (m2.xy, exp2(scale), 0)
  std::array<int, MaxTextures> sizeBias{}; // (width, height, lodBias, 0) per sampled texture
  uint16_t total = 0;
};

BlobLayout make_blob_layout(const ShaderConfig& cfg, const ShaderInfo& info) {
  BlobLayout b;
  b.sizeBias.fill(-1);
  uint16_t offset = 0;
  const auto take = [&offset](uint32_t n) {
    const auto at = offset;
    offset = static_cast<uint16_t>(offset + n);
    return at;
  };
  b.viewport = take(1);
  if (info.lineMode != 0) {
    b.lineParams = take(1);
  }
  b.proj = take(4);
  b.pnPos = take(MaxPnMtx * 3);
  b.texMtx = take(MaxTexMtx * 3);
  b.pnNrm = take(MaxPnMtx * 3);
  for (uint32_t i = 0; i < MaxTevRegs; ++i) {
    if (info.loadsTevReg.test(i)) {
      b.tevReg[i] = take(1);
    }
  }
  if (info.lightingEnabled) {
    b.lights = take(GX::MaxLights * kLightVec4);
    b.lightState = take(1);
  }
  for (uint32_t i = 0; i < b.chan.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& cc = cfg.colorChannels[i];
    const auto& cca = cfg.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      b.chan[i].amb = take(1);
    }
    if (cc.matSrc == GX_SRC_REG) {
      b.chan[i].mat = take(1);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      b.chan[i].ambA = take(1);
    }
    if (cca.matSrc == GX_SRC_REG) {
      b.chan[i].matA = take(1);
    }
  }
  for (uint32_t i = 0; i < MaxKColors; ++i) {
    if (info.sampledKColors.test(i)) {
      b.kcolor[i] = take(1);
    }
  }
  if (info.usesPTTexMtx.any()) {
    b.ptTexMtx = take(MaxPTTexMtx * 3);
  }
  if (info.usesFog) {
    b.fog = take(kFogVec4);
  }
  b.texcoordScale = take(MaxTexCoord);
  if (info.usedIndTexMtxs.any()) {
    b.indMtx = take(MaxIndTexMtxs * 2);
  }
  for (uint32_t i = 0; i < MaxTextures; ++i) {
    if (info.sampledTextures.test(i)) {
      b.sizeBias[i] = take(1);
    }
  }
  b.total = offset;
  return b;
}

// ---- per-stage uniform array allocation -------------------------------------------------------
struct StageArray {
  uint16_t size = 0;
  std::vector<UniformSeg> segs;

  // Reserves `count` vec4s that mirror blob[src ...] and returns their position in the array. `intLanes`
  // marks lanes that hold u32 bit fields in the blob (see UniformSeg); segments only merge when equal.
  uint16_t alloc(uint32_t src, uint32_t count, uint8_t intLanes = 0) {
    const auto dst = size;
    if (!segs.empty() && segs.back().src + segs.back().count == src &&
        segs.back().dst + segs.back().count == dst && segs.back().intLanes == intLanes) {
      segs.back().count = static_cast<uint16_t>(segs.back().count + count);
    } else {
      segs.push_back({static_cast<uint16_t>(src), dst, static_cast<uint16_t>(count), intLanes});
    }
    size = static_cast<uint16_t>(size + count);
    return dst;
  }
};

// Positions of the blob pieces inside uVS / uFS (-1 = not present in that stage).
struct VsU {
  int viewport = -1;
  int lineParams = -1;
  int proj = -1;
  int pnPos = -1;
  int texMtxAll = -1;                 // all MaxTexMtx matrices (dynamic matrix index), else -1
  std::array<int, MaxTexMtx> texMtx{}; // per static matrix
  int pnNrm = -1;
  int lights = -1;     // all lights contiguous
  std::array<int, GX::MaxLights> light{}; // per light (emboss only)
  int lightState = -1;
  std::array<BlobLayout::Chan, MaxColorChannels / 2> chan{};
  std::array<int, MaxPTTexMtx> ptMtx{};
};
struct FsU {
  std::array<int, MaxTevRegs> tevReg{-1, -1, -1, -1};
  std::array<int, MaxKColors> kcolor{-1, -1, -1, -1};
  int fog = -1;
  std::array<int, MaxTexCoord> texScale{};
  std::array<int, MaxIndTexMtxs> indMtx{};
  std::array<int, MaxTextures> sizeBias{};
};

// ---- varying packing ---------------------------------------------------------------------------
struct VaryingItem {
  std::string name; // variable name used by both stages, e.g. "cc0" / "tex2"
  int comps;        // 2, 3 or 4
};
struct VaryingSlot {
  std::string var;  // "vCc0" (unpacked) or "vP3" (packed)
  std::string swiz; // ".xy" ... or "" when the whole vec4/vec3/vec2 is the item
  int comps;
};

class Builder {
public:
  Builder(const PipelineConfig& pc, const ShaderInfo& info, bool pnMtxZero, uint32_t lights)
  : m_pc(pc), m_cfg(pc.shaderConfig), m_info(info), m_blob(make_blob_layout(pc.shaderConfig, info)),
    // Never trust the flag over the vertex format: a per-vertex matrix index can select any bank.
    m_fullPnBanks(!pnMtxZero || pc.shaderConfig.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE),
    m_lights(std::clamp<uint32_t>(lights, 1, GX::MaxLights)) {
    const bool usesSrcAlpha = [&] {
      const auto srcAlpha = [](GXBlendFactor f) { return f == GX_BL_SRCALPHA || f == GX_BL_INVSRCALPHA; };
      return pc.colorUpdate && pc.blendMode == GX_BM_BLEND && (srcAlpha(pc.blendFacSrc) || srcAlpha(pc.blendFacDst));
    }();
    m_replaceAlpha = pc.alphaUpdate && pc.dstAlpha != UINT32_MAX && pc.dstAlpha != 0 && !usesSrcAlpha;
    m_layout = decoded_layout(m_cfg);
  }

  ProgramSource build();

private:
  // setup
  void analyze();
  void allocate_uniforms();
  void plan_varyings();

  // vertex shader
  std::string vtx_attr(GXAttr attr);
  std::string attr_ref(const char* name, const char* fallback);
  std::string build_vertex();
  std::string lighting_func(const ColorChannelConfig& cc, uint32_t i, bool alpha);
  void gen_texgen(std::string& body, std::vector<std::pair<std::string, std::string>>& varyingWrites);

  // fragment shader
  std::string build_fragment();
  std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const TevStage& stage);
  std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const TevStage& stage);
  std::string alpha_bump_sel(size_t stageIdx, const TevStage& stage);
  std::string tev_op(GXTevOp op, std::string_view bias, std::string_view scale, std::string_view a,
                     std::string_view b, std::string_view c, std::string_view d, bool isColor);
  std::string kcolor_ref(int idx);
  std::string sample(uint32_t texMap, const std::string& uv);
  std::string tex_size(uint32_t texMap);

  const VaryingSlot& varying(const std::string& name) const;

  const PipelineConfig& m_pc;
  const ShaderConfig& m_cfg;
  const ShaderInfo& m_info;
  BlobLayout m_blob;
  DecodedLayout m_layout;
  bool m_replaceAlpha = false;

  // analysis
  bool m_chanLit = false;
  bool m_emboss = false;
  bool m_dynTexMtx = false;
  std::bitset<MaxTexMtx> m_staticTexMtx;
  std::bitset<GX::MaxLights> m_embossLights;
  bool m_needNrm = false;
  // False only when the caller guarantees aPnMtxIdx == 0 for every vertex (pn_mtx_provably_zero): then
  // one 3-vec4 bank is enough. Otherwise all MaxPnMtx banks are uploaded. Const-initialised in the ctor.
  const bool m_fullPnBanks;
  uint32_t m_lights; // light slots the shader reads (1..MaxLights); raised in analyze() for emboss lights
  std::bitset<MaxTexCoord> m_needTexScale;
  std::bitset<MaxTextures> m_needTexSize;
  std::bitset<MaxTexCoord> m_embossTexCoord;

  StageArray m_vs;
  StageArray m_fs;
  VsU m_v;
  FsU m_f;

  std::vector<VaryingItem> m_varyItems;
  std::vector<std::pair<std::string, VaryingSlot>> m_varySlots;
  uint32_t m_varyVec4 = 0;
  bool m_packed = false;

  // generation state
  std::vector<std::string> m_usedAttrs; // attribute names referenced by the vertex shader
  std::bitset<MaxTextures> m_usedTextures;
  bool m_needRound = false;
  bool m_needOverflow = false;
  bool m_needFmod = false;
};

// =================================================================================================
// analysis & allocation
// =================================================================================================
void Builder::analyze() {
  for (uint32_t i = 0; i < m_blob.chan.size(); ++i) {
    if (m_info.sampledColorChannels.test(i) &&
        (m_cfg.colorChannels[i].lightingEnabled || m_cfg.colorChannels[i + GX_ALPHA0].lightingEnabled)) {
      m_chanLit = true;
    }
  }
  for (uint32_t i = 0; i < MaxTexCoord; ++i) {
    if (!m_info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = m_cfg.tcgs[i];
    if (is_emboss_texgen(tcg.type)) {
      m_emboss = true;
      m_embossTexCoord.set(i);
      m_embossLights.set(std::min<uint32_t>(tcg.type - GX_TG_BUMP0, GX::MaxLights - 1));
      continue;
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (m_cfg.attrs[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE) {
        m_dynTexMtx = true;
      } else if (tcg.mtx >= GX_TEXMTX0 && tcg.mtx < GX_TEXMTX0 + 3 * MaxTexMtx) {
        m_staticTexMtx.set((tcg.mtx - GX_TEXMTX0) / 3);
      }
    }
  }
  m_needNrm = m_chanLit || m_emboss;
  // Emboss texgens name their light directly (not through a mask), so they must lie inside the slots.
  if (m_chanLit) {
    for (uint32_t l = 0; l < GX::MaxLights; ++l) {
      if (m_embossLights.test(l)) {
        m_lights = std::max(m_lights, l + 1);
      }
    }
  }
  // Matrix banks: aPnMtxIdx is ALWAYS a decoded attribute (vita_gx.hpp): the per-vertex index with
  // GX_VA_PNMTXIDX, otherwise the constant GXSetCurrentMtx value of the draw. It selects any of the
  // MaxPnMtx matrices, so the full banks are the default. When the caller proves it is 0
  // (m_fullPnBanks == false) only bank 0 is read; that saves ~54 vec4 for the common unskinned draw
  // and keeps lit + dynamic-tex-matrix shaders within vitaGL's 128 vertex-vec4 cap.
  // Indirect texturing: which texcoord scales / texture sizes does the fragment shader read?
  for (uint32_t i = 0; i < m_cfg.tevStageCount; ++i) {
    const auto& stage = m_cfg.tevStages[i];
    const bool hasIndStage = stage.indTexStage < m_cfg.numIndStages;
    if (hasIndStage && (stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId))) {
      const auto& indStage = m_cfg.indStages[stage.indTexStage];
      m_needTexScale.set(indStage.texCoordId);
      m_needTexSize.set(indStage.texMapId);
    }
    const bool simple = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;
    const bool needsTevCoord = stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
                               stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    if (needsTevCoord && !simple) {
      if (stage.texCoordId != GX_TEXCOORD_NULL) {
        m_needTexScale.set(stage.texCoordId);
      }
      if (stage.texMapId != GX_TEXMAP_NULL) {
        m_needTexSize.set(stage.texMapId);
      }
    }
  }
  if (kUseTexLodBias) {
    m_needTexSize |= m_info.sampledTextures;
  }
}

void Builder::allocate_uniforms() {
  const auto& b = m_blob;
  const bool line = m_info.lineMode != 0;
  m_v.texMtx.fill(-1);
  m_v.light.fill(-1);
  m_v.ptMtx.fill(-1);
  m_f.texScale.fill(-1);
  m_f.indMtx.fill(-1);
  m_f.sizeBias.fill(-1);

  // Walk the blob in order so neighbouring pieces merge into one segment.
  if (line) {
    m_v.viewport = m_vs.alloc(b.viewport, 1);
    m_v.lineParams = m_vs.alloc(static_cast<uint32_t>(b.lineParams), 1, 0x8); // .w = texcoord mask (u32)
  }
  // One 3-vec4 bank when every vertex reads matrix 0 (see analyze()), else all MaxPnMtx banks. The
  // uniform blob (m_blob) keeps the upstream layout, so both stages stay coherent through UniformSeg
  // remapping.
  const uint32_t pnBanks = m_fullPnBanks ? MaxPnMtx * 3u : 3u;
  m_v.proj = m_vs.alloc(b.proj, 4);
  m_v.pnPos = m_vs.alloc(b.pnPos, pnBanks);
  if (m_dynTexMtx) {
    m_v.texMtxAll = m_vs.alloc(b.texMtx, MaxTexMtx * 3);
  } else {
    for (uint32_t i = 0; i < MaxTexMtx; ++i) {
      if (m_staticTexMtx.test(i)) {
        m_v.texMtx[i] = m_vs.alloc(b.texMtx + i * 3, 3);
      }
    }
  }
  if (m_needNrm) {
    m_v.pnNrm = m_vs.alloc(b.pnNrm, pnBanks);
  }
  for (uint32_t i = 0; i < MaxTevRegs; ++i) {
    if (b.tevReg[i] >= 0) {
      m_f.tevReg[i] = m_fs.alloc(static_cast<uint32_t>(b.tevReg[i]), 1);
    }
  }
  if (b.lights >= 0) {
    if (m_chanLit) {
      m_v.lights = m_vs.alloc(static_cast<uint32_t>(b.lights), m_lights * kLightVec4);
    } else {
      for (uint32_t l = 0; l < GX::MaxLights; ++l) {
        if (m_embossLights.test(l)) {
          m_v.light[l] = m_vs.alloc(static_cast<uint32_t>(b.lights) + l * kLightVec4, kLightVec4);
        }
      }
    }
    if (m_chanLit) {
      m_v.lightState = m_vs.alloc(static_cast<uint32_t>(b.lightState), 1, 0xF); // 4 light masks (u32)
    }
  }
  for (uint32_t i = 0; i < b.chan.size(); ++i) {
    const auto& c = b.chan[i];
    auto& v = m_v.chan[i];
    if (c.amb >= 0) {
      v.amb = m_vs.alloc(static_cast<uint32_t>(c.amb), 1);
    }
    if (c.mat >= 0) {
      v.mat = m_vs.alloc(static_cast<uint32_t>(c.mat), 1);
    }
    if (c.ambA >= 0) {
      v.ambA = m_vs.alloc(static_cast<uint32_t>(c.ambA), 1);
    }
    if (c.matA >= 0) {
      v.matA = m_vs.alloc(static_cast<uint32_t>(c.matA), 1);
    }
  }
  for (uint32_t i = 0; i < MaxKColors; ++i) {
    if (b.kcolor[i] >= 0) {
      m_f.kcolor[i] = m_fs.alloc(static_cast<uint32_t>(b.kcolor[i]), 1);
    }
  }
  if (b.ptTexMtx >= 0) {
    for (uint32_t i = 0; i < MaxPTTexMtx; ++i) {
      if (m_info.usesPTTexMtx.test(i)) {
        m_v.ptMtx[i] = m_vs.alloc(static_cast<uint32_t>(b.ptTexMtx) + i * 3, 3);
      }
    }
  }
  if (b.fog >= 0) {
    // Only the colour and (a, b, c) vec4s; the range table travels as a texture (uFogRange), not as uniforms.
    m_f.fog = m_fs.alloc(static_cast<uint32_t>(b.fog), 2);
  }
  for (uint32_t i = 0; i < MaxTexCoord; ++i) {
    if (m_needTexScale.test(i)) {
      m_f.texScale[i] = m_fs.alloc(b.texcoordScale + i, 1);
    }
  }
  if (b.indMtx >= 0) {
    for (uint32_t i = 0; i < MaxIndTexMtxs; ++i) {
      if (m_info.usedIndTexMtxs.test(i)) {
        m_f.indMtx[i] = m_fs.alloc(static_cast<uint32_t>(b.indMtx) + i * 2, 2);
      }
    }
  }
  for (uint32_t i = 0; i < MaxTextures; ++i) {
    if (b.sizeBias[i] >= 0 && m_needTexSize.test(i)) {
      m_f.sizeBias[i] = m_fs.alloc(static_cast<uint32_t>(b.sizeBias[i]), 1);
    }
  }
}

// =================================================================================================
// varyings
// =================================================================================================
void Builder::plan_varyings() {
  for (uint32_t i = 0; i < m_info.sampledColorChannels.size(); ++i) {
    if (m_info.sampledColorChannels.test(i)) {
      m_varyItems.push_back({fmt::format("cc{}", i), 4});
    }
  }
  for (uint32_t i = 0; i < MaxTexCoord; ++i) {
    if (!m_info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = m_cfg.tcgs[i];
    const bool uvw = !is_emboss_texgen(tcg.type) && tcg.type == GX_TG_MTX3x4;
    m_varyItems.push_back({fmt::format("tex{}", i), uvw ? 3 : 2});
  }
  uint32_t plain = static_cast<uint32_t>(m_varyItems.size());
  m_packed = plain > static_cast<uint32_t>(kMaxVaryingVec4);
  if (!m_packed) {
    for (const auto& item : m_varyItems) {
      const auto var = fmt::format("v{}{}", static_cast<char>(std::toupper(static_cast<unsigned char>(item.name[0]))),
                                   item.name.substr(1));
      m_varySlots.push_back({item.name, VaryingSlot{var, "", item.comps}});
    }
    m_varyVec4 = plain;
    return;
  }
  // Pack: vec4s alone, then vec3s (one spare lane), then vec2 pairs.
  struct Slot {
    int used = 0;
  };
  std::vector<Slot> slots;
  const auto place = [&](const VaryingItem& item) {
    if (item.comps == 2) {
      for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].used == 2) {
          m_varySlots.push_back({item.name, {fmt::format("vP{}", s), ".zw", 2}});
          slots[s].used = 4;
          return;
        }
      }
    }
    slots.push_back({item.comps});
    const std::string swiz = item.comps == 4 ? "" : item.comps == 3 ? ".xyz" : ".xy";
    m_varySlots.push_back({item.name, {fmt::format("vP{}", slots.size() - 1), swiz, item.comps}});
  };
  for (int comps = 4; comps >= 2; --comps) {
    for (const auto& item : m_varyItems) {
      if (item.comps == comps) {
        place(item);
      }
    }
  }
  m_varyVec4 = static_cast<uint32_t>(slots.size());
}

const VaryingSlot& Builder::varying(const std::string& name) const {
  for (const auto& [n, slot] : m_varySlots) {
    if (n == name) {
      return slot;
    }
  }
  Log.fatal("vita shader: unknown varying {}", name);
}

// =================================================================================================
// vertex shader
// =================================================================================================
std::string Builder::attr_ref(const char* name, const char* fallback) {
  for (uint32_t i = 0; i < m_layout.count; ++i) {
    if (std::string_view(m_layout.attrs[i].name) == name) {
      if (std::find(m_usedAttrs.begin(), m_usedAttrs.end(), name) == m_usedAttrs.end()) {
        m_usedAttrs.emplace_back(name);
      }
      return name;
    }
  }
  return fallback;
}

// Decoded value of a GX vertex attribute (as upstream's vtx_attr: default values for absent ones).
std::string Builder::vtx_attr(GXAttr attr) {
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return attr_ref("aPnMtxIdx", "0.0");
  case GX_VA_POS:
    return attr_ref("aPos", "vec3(0.0)");
  case GX_VA_NRM:
    return attr_ref("aNrm", "vec3(1.0, 0.0, 0.0)");
  case GX_VA_CLR0:
    return attr_ref("aClr0", "vec4(0.0)");
  case GX_VA_CLR1:
    return attr_ref("aClr1", "vec4(0.0)");
  default:
    break;
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto n = attr - GX_VA_TEX0;
    static constexpr std::array<const char*, 8> names{"aTex0", "aTex1", "aTex2", "aTex3",
                                                      "aTex4", "aTex5", "aTex6", "aTex7"};
    return attr_ref(names[n], "vec2(0.0)");
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto n = attr - GX_VA_TEX0MTXIDX;
    static constexpr std::array<const char*, 8> names{"aTexMtxIdx0", "aTexMtxIdx1", "aTexMtxIdx2", "aTexMtxIdx3",
                                                      "aTexMtxIdx4", "aTexMtxIdx5", "aTexMtxIdx6", "aTexMtxIdx7"};
    return attr_ref(names[n], "0.0");
  }
  Log.fatal("vita shader: unhandled vertex attribute {}", underlying(attr));
}

std::string Builder::lighting_func(const ColorChannelConfig& cc, uint32_t i, bool alpha) {
  const std::string_view swizzle = alpha ? ".a"sv : ""sv;
  const auto& vc = m_v.chan[i];
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    ambSrc = vtx_attr(static_cast<GXAttr>(GX_VA_CLR0 + i));
  } else if (cc.ambSrc == GX_SRC_REG) {
    const int at = alpha ? vc.ambA : vc.amb;
    ambSrc = at >= 0 ? fmt::format("uVS[{}]", at) : "vec4(0.0)"s;
  }
  if (cc.matSrc == GX_SRC_VTX) {
    matSrc = vtx_attr(static_cast<GXAttr>(GX_VA_CLR0 + i));
  } else if (cc.matSrc == GX_SRC_REG) {
    const int at = alpha ? vc.matA : vc.mat;
    matSrc = at >= 0 ? fmt::format("uVS[{}]", at) : "vec4(1.0)"s;
  }
  const auto out = fmt::format("cc{}", i);
  if (!cc.lightingEnabled) {
    return fmt::format("\n  {0}{2} = {1}{2};", out, matSrc, swizzle);
  }

  const GXDiffuseFn diffFn = cc.diffFn;
  std::string attnFn;
  if (cc.attnFn == GX_AF_NONE) {
    attnFn = "\n      attn = 1.0;";
  } else if (cc.attnFn == GX_AF_SPOT) {
    attnFn =
        "\n      float cosine = max(0.0, dot(ldir, ldirL));"
        "\n      float cos_attn = dot(cosAtt, vec3(1.0, cosine, cosine * cosine));"
        "\n      float dist_attn = dot(distAtt, vec3(1.0, dist, dist2));"
        "\n      attn = max(0.0, cos_attn / dist_attn);";
  } else if (cc.attnFn == GX_AF_SPEC) {
    const std::string distAttn = diffFn != GX_DF_NONE
                                     ? "max(0.0, dot(normalize(distAtt), vec3(1.0, attn, attn * attn)));"
                                     : "max(0.0, dot(distAtt, vec3(1.0, attn, attn * attn)));";
    attnFn = fmt::format(
        "\n      attn = (dot(mv_nrm, ldir) >= 0.0) ? max(0.0, dot(mv_nrm, ldirL)) : 0.0;"
        "\n      float cos_attn = dot(cosAtt, vec3(1.0, attn, attn * attn));"
        "\n      float dist_attn = {};"
        "\n      attn = max(0.0, cos_attn / dist_attn);",
        distAttn);
  }
  std::string diffExpr;
  if (diffFn == GX_DF_NONE) {
    diffExpr = "1.0";
  } else if (diffFn == GX_DF_SIGN) {
    diffExpr = "dot(ldir, mv_nrm)";
  } else {
    diffExpr = "max(0.0, dot(ldir, mv_nrm))";
  }
  // Light mask lane of this channel: COLOR0, COLOR1, ALPHA0, ALPHA1.
  static constexpr std::array<char, 4> lanes{'x', 'y', 'z', 'w'};
  const char lane = lanes[i + (alpha ? 2 : 0)];
  return fmt::format(
      "\n  {{"
      "\n    vec4 lighting = {amb};"
      "\n    float lmask = uVS[{ls}].{lane};"
      "\n    for (int li = 0; li < {nl}; li++) {{"
      "\n      float lbit = mod(lmask, 2.0);"
      "\n      lmask = floor(lmask * 0.5);"
      "\n      if (lbit < 0.5) {{ continue; }}"
      "\n      int lb = {lb} + li * {lsz};"
      "\n      vec3 lpos = uVS[lb].xyz;"
      "\n      vec3 ldirL = uVS[lb + 1].xyz;"
      "\n      vec4 lcolor = uVS[lb + 2];"
      "\n      vec3 cosAtt = uVS[lb + 3].xyz;"
      "\n      vec3 distAtt = uVS[lb + 4].xyz;"
      "\n      vec3 ldir = lpos - mv_pos;"
      "\n      float dist2 = dot(ldir, ldir);"
      "\n      float dist = sqrt(dist2);"
      "\n      ldir = ldir / dist;"
      "\n      float attn;{attn}"
      "\n      float diff = {diff};"
      "\n      lighting = lighting + (attn * diff * lcolor);"
      "\n    }}"
      "\n    {out}{sw} = ({mat} * clamp(lighting, vec4(0.0), vec4(1.0))){sw};"
      "\n  }}",
      fmt::arg("amb", ambSrc), fmt::arg("ls", m_v.lightState), fmt::arg("lane", lane), fmt::arg("nl", m_lights),
      fmt::arg("lb", m_v.lights), fmt::arg("lsz", kLightVec4), fmt::arg("attn", attnFn), fmt::arg("diff", diffExpr),
      fmt::arg("out", out), fmt::arg("mat", matSrc), fmt::arg("sw", swizzle));
}

std::string Builder::build_vertex() {
  const auto& cfg = m_cfg;
  const int lineMode = cfg.lineMode;
  std::string pre;  // transforms
  std::string body; // channels + texgen
  std::vector<std::pair<std::string, std::string>> writes; // (varying name, expression)

  // --- position -------------------------------------------------------------------------------
  if (lineMode == 0) {
    pre += fmt::format("\n  int pnB = int({} + 0.5) * 3;", vtx_attr(GX_VA_PNMTXIDX));
    pre += fmt::format("\n  vec3 mv_pos = tpv_mul43(vec4({}, 1.0), {} + pnB);", vtx_attr(GX_VA_POS), m_v.pnPos);
    pre += fmt::format(
        "\n  vec4 mvp = vec4(mv_pos, 1.0);"
        "\n  vec4 clip = vec4(dot(mvp, uVS[{0}]), dot(mvp, uVS[{1}]), dot(mvp, uVS[{2}]), dot(mvp, uVS[{3}]));",
        m_v.proj, m_v.proj + 1, m_v.proj + 2, m_v.proj + 3);
  } else {
    const std::string corner = attr_ref("aCorner", "0.0");
    pre += fmt::format("\n  float corner = {};", corner);
    pre += "\n  bool use_b = corner >= 1.5;";
    pre += "\n  float side = mod(corner, 2.0);"; // 0 / 1 : (vidx & 1)
    pre += fmt::format("\n  int pnB = int({} + 0.5) * 3;", vtx_attr(GX_VA_PNMTXIDX));
    pre += fmt::format("\n  vec3 mv_pos = tpv_mul43(vec4({}, 1.0), {} + pnB);", vtx_attr(GX_VA_POS), m_v.pnPos);
    pre += fmt::format("\n  vec4 mvp = vec4(mv_pos, 1.0);");
    pre += fmt::format(
        "\n  vec4 clip_own = vec4(dot(mvp, uVS[{0}]), dot(mvp, uVS[{1}]), dot(mvp, uVS[{2}]), dot(mvp, uVS[{3}]));",
        m_v.proj, m_v.proj + 1, m_v.proj + 2, m_v.proj + 3);
    pre += fmt::format("\n  vec2 rvp = uVS[{0}].xy;\n  vec2 lvp = uVS[{0}].zw;", m_v.viewport);
    pre += "\n  vec2 viewport_scale = rvp / max(lvp, vec2(1.0));";
    pre += fmt::format("\n  float line_width = uVS[{}].x;", m_v.lineParams);
    pre += fmt::format("\n  float line_tex_offset = uVS[{}].z;", m_v.lineParams);
    pre += fmt::format("\n  float line_tex_mask = uVS[{}].w;", m_v.lineParams);
    if (lineMode == 3) {
      // GX_POINTS: expand one vertex into an axis-aligned screen-space square.
      pre +=
          "\n  vec4 clip = clip_own;"
          "\n  float point_size = line_width * min(viewport_scale.x, viewport_scale.y);"
          "\n  float x_sign = side >= 0.5 ? 1.0 : -1.0;"
          "\n  float y_sign = corner >= 1.5 ? 1.0 : -1.0;"
          "\n  vec2 offset_px = vec2(x_sign, y_sign) * (point_size / 2.0);"
          "\n  vec2 offset_ndc = (offset_px * 2.0) / rvp;"
          "\n  clip = vec4(clip.xy + offset_ndc * clip.w, clip.zw);";
    } else {
      // GX_LINES / GX_LINESTRIP: widen the segment perpendicular to its screen-space direction.
      pre += fmt::format("\n  int pnBo = int({} + 0.5) * 3;", attr_ref("aPnMtxIdxOther", "0.0"));
      pre += fmt::format("\n  vec3 mv_pos_o = tpv_mul43(vec4({}, 1.0), {} + pnBo);",
                         attr_ref("aPosOther", "vec3(0.0)"), m_v.pnPos);
      pre += "\n  vec4 mvpo = vec4(mv_pos_o, 1.0);";
      pre += fmt::format(
          "\n  vec4 clip_oth = vec4(dot(mvpo, uVS[{0}]), dot(mvpo, uVS[{1}]), dot(mvpo, uVS[{2}]), dot(mvpo, "
          "uVS[{3}]));",
          m_v.proj, m_v.proj + 1, m_v.proj + 2, m_v.proj + 3);
      pre +=
          "\n  vec4 clip_a = use_b ? clip_oth : clip_own;"
          "\n  vec4 clip_b = use_b ? clip_own : clip_oth;"
          "\n  vec2 ndc_a = clip_a.xy / clip_a.w;"
          "\n  vec2 ndc_b = clip_b.xy / clip_b.w;"
          "\n  vec2 delta_px = (ndc_b - ndc_a) / 2.0 * rvp;"
          "\n  vec2 dir_px = dot(delta_px, delta_px) > 1e-10 ? normalize(delta_px) : vec2(1.0, 0.0);"
          "\n  vec2 perp_px = vec2(-dir_px.y, dir_px.x);"
          "\n  float lw = line_width * min(viewport_scale.x, viewport_scale.y);"
          "\n  vec2 offset_px = perp_px * (lw / 2.0) * (side >= 0.5 ? 1.0 : -1.0);"
          "\n  vec2 offset_ndc = (offset_px * 2.0) / rvp;"
          "\n  vec4 clip = vec4(clip_own.xy + offset_ndc * clip_own.w, clip_own.zw);";
    }
  }

  // --- normal ---------------------------------------------------------------------------------
  if (m_needNrm) {
    pre += fmt::format("\n  vec3 nrm_tmp = tpv_mul43(vec4({}, 0.0), {} + pnB);", vtx_attr(GX_VA_NRM), m_v.pnNrm);
    pre += "\n  vec3 mv_nrm = dot(nrm_tmp, nrm_tmp) > 1e-10 ? normalize(nrm_tmp) : nrm_tmp;";
  }

  // --- channels -------------------------------------------------------------------------------
  for (uint32_t i = 0; i < m_info.sampledColorChannels.size(); ++i) {
    if (!m_info.sampledColorChannels.test(i)) {
      continue;
    }
    body += fmt::format("\n  vec4 cc{};", i);
    body += lighting_func(m_cfg.colorChannels[i], i, false);
    body += lighting_func(m_cfg.colorChannels[i + GX_ALPHA0], i, true);
    writes.emplace_back(fmt::format("cc{}", i), fmt::format("cc{}", i));
  }

  // --- texture coordinates --------------------------------------------------------------------
  gen_texgen(body, writes);

  // --- varyings -------------------------------------------------------------------------------
  std::string varyDecl;
  std::string varyWrite;
  if (!m_packed) {
    for (const auto& [name, slot] : m_varySlots) {
      const int comps = slot.comps;
      varyDecl += fmt::format("\nvarying {} {};", comps == 4 ? "vec4" : comps == 3 ? "vec3" : "vec2", slot.var);
    }
  } else {
    for (uint32_t s = 0; s < m_varyVec4; ++s) {
      varyDecl += fmt::format("\nvarying vec4 vP{};", s);
    }
  }
  for (const auto& [name, expr] : writes) {
    const auto& slot = varying(name);
    varyWrite += fmt::format("\n  {}{} = {};", slot.var, slot.swiz, expr);
  }

  // --- assemble -------------------------------------------------------------------------------
  std::string attrDecl;
  for (uint32_t i = 0; i < m_layout.count; ++i) {
    const auto& a = m_layout.attrs[i];
    if (std::find(m_usedAttrs.begin(), m_usedAttrs.end(), a.name) == m_usedAttrs.end()) {
      continue;
    }
    const char* type = a.kind == DecodedAttr::Kind::UByte4Norm ? "vec4"
                       : a.components == 1                      ? "float"
                       : a.components == 2                      ? "vec2"
                       : a.components == 3                      ? "vec3"
                                                                : "vec4";
    attrDecl += fmt::format("\nattribute {} {};", type, a.name);
  }

  std::string src;
  src.reserve(4096);
  src += "// GX vertex shader (generated by vita_shader.cpp)\n";
  src += fmt::format("uniform vec4 uVS[{}];", m_vs.size);
  src += attrDecl;
  src += varyDecl;
  src +=
      "\nvec3 tpv_mul43(vec4 v, int b) {"
      "\n  return vec3(dot(v, uVS[b]), dot(v, uVS[b + 1]), dot(v, uVS[b + 2]));"
      "\n}\n"
      "\nvoid main() {";
  src += pre;
  src += body;
  src += varyWrite;
  src +=
      "\n  // WebGPU clip space (z in [0,w], y up/row 0 on top) -> GL (z in [-w,w]); Y flipped so that FBO row 0"
      "\n  // is the GX top row."
      "\n  gl_Position = vec4(clip.x, -clip.y, 2.0 * clip.z - clip.w, clip.w);"
      "\n}\n";
  return src;
}

void Builder::gen_texgen(std::string& body, std::vector<std::pair<std::string, std::string>>& writes) {
  const auto& cfg = m_cfg;
  const bool line = cfg.lineMode != 0;
  for (uint32_t i = 0; i < MaxTexCoord; ++i) {
    if (!m_info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = cfg.tcgs[i];
    if (is_emboss_texgen(tcg.type)) {
      // Emboss bump: offset the source texcoord by the light projected onto tangent/binormal.
      const uint32_t lightIdx = std::min<uint32_t>(tcg.type - GX_TG_BUMP0, GX::MaxLights - 1);
      const auto tan = attr_ref("aTan", "vec3(0.0)");
      const auto bin = attr_ref("aBin", "vec3(0.0)");
      const int lightAt = m_chanLit ? m_v.lights + static_cast<int>(lightIdx * kLightVec4) : m_v.light[lightIdx];
      body += fmt::format(
          "\n  vec3 bump_ldir{0} = normalize(uVS[{1}].xyz - mv_pos);"
          "\n  vec3 bump_tan{0} = tpv_mul43(vec4({2}, 0.0), {4} + pnB);"
          "\n  vec3 bump_bin{0} = tpv_mul43(vec4({3}, 0.0), {4} + pnB);"
          "\n  vec2 tc{0}_uv = tc{5}_proj.xy + vec2(dot(bump_ldir{0}, bump_tan{0}), dot(bump_ldir{0}, bump_bin{0}));",
          i, lightAt, tan, bin, m_v.pnNrm, tcg.embossSrc);
      writes.emplace_back(fmt::format("tex{}", i), fmt::format("tc{}_uv", i));
      continue;
    }

    // source
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      body += fmt::format("\n  vec4 tc{} = vec4({}, 1.0, 1.0);", i,
                          vtx_attr(static_cast<GXAttr>(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_TG_POS) {
      body += fmt::format("\n  vec4 tc{} = vec4({}, 1.0);", i, vtx_attr(GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      body += fmt::format("\n  vec4 tc{} = vec4({}, 1.0);", i, vtx_attr(GX_VA_NRM));
    } else if (tcg.src == GX_TG_COLOR0) {
      body += fmt::format("\n  vec4 tc{} = {};", i, vtx_attr(GX_VA_CLR0));
    } else if (tcg.src == GX_TG_COLOR1) {
      body += fmt::format("\n  vec4 tc{} = {};", i, vtx_attr(GX_VA_CLR1));
    } else if (tcg.src == GX_TG_BINRM) {
      body += fmt::format("\n  vec4 tc{} = vec4({}, 1.0);", i, attr_ref("aBin", "vec3(0.0)"));
    } else if (tcg.src == GX_TG_TANGENT) {
      body += fmt::format("\n  vec4 tc{} = vec4({}, 1.0);", i, attr_ref("aTan", "vec3(0.0)"));
    } else {
      body += fmt::format("\n  vec4 tc{} = vec4(0.0, 0.0, 1.0, 1.0);", i);
    }

    // matrix
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (cfg.attrs[GX_VA_TEX0MTXIDX + i].attrType != GX_NONE) {
        // The decoder stores the raw GX byte (GX_TEXMTX0 + 3k = 30 + 3k, upstream divides it by 3 and
        // indexes after the 10 position matrices); a plain 0..9 slot number is accepted as well.
        body += fmt::format(
            "\n  int tmi{0} = int({2} + 0.5);"
            "\n  tmi{0} = tmi{0} >= {3} ? (tmi{0} - {3}) / 3 : tmi{0};"
            "\n  vec3 tc{0}_tmp = tpv_mul43(tc{0}, {1} + tmi{0} * 3);",
            i, m_v.texMtxAll, vtx_attr(static_cast<GXAttr>(GX_VA_TEX0MTXIDX + i)), static_cast<int>(GX_TEXMTX0));
      } else if (tcg.mtx >= GX_TEXMTX0 && tcg.mtx < GX_TEXMTX0 + 3 * MaxTexMtx) {
        body += fmt::format("\n  vec3 tc{0}_tmp = tpv_mul43(tc{0}, {1});", i, m_v.texMtx[(tcg.mtx - GX_TEXMTX0) / 3]);
      } else {
        body += fmt::format("\n  vec3 tc{0}_tmp = tc{0}.xyz;", i);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        body += fmt::format("\n  tc{0}_tmp.z = 1.0;", i);
      }
    } else if (tcg.type == GX_TG_SRTG) {
      body += fmt::format("\n  vec3 tc{0}_tmp = vec3(tc{0}.xy, 1.0);", i);
    } else {
      body += fmt::format("\n  vec3 tc{0}_tmp = tc{0}.xyz;", i);
    }
    if (tcg.normalize) {
      body += fmt::format("\n  tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (tcg.postMtx == GX_PTIDENTITY) {
      body += fmt::format("\n  vec3 tc{0}_proj = tc{0}_tmp;", i);
    } else {
      const uint32_t postIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      body += fmt::format("\n  vec3 tc{0}_proj = tpv_mul43(vec4(tc{0}_tmp.xyz, 1.0), {1});", i,
                          postIdx < MaxPTTexMtx && m_v.ptMtx[postIdx] >= 0 ? m_v.ptMtx[postIdx] : 0);
    }
    // line / point texture offset (the mask lane is a bit set of texcoords; the u32 from the blob is converted
    // to a float at upload, see UniformSeg::intLanes)
    if (line) {
      const uint32_t bit = 1u << i;
      const auto maskTest = fmt::format("mod(floor(line_tex_mask / {}.0), 2.0) >= 0.5", bit);
      if (cfg.lineMode == 3) {
        body += fmt::format(
            "\n  if ({1}) {{"
            "\n    if (side >= 0.5) {{ tc{0}_proj.x += line_tex_offset; }}"
            "\n    if (corner >= 1.5) {{ tc{0}_proj.y += line_tex_offset; }}"
            "\n  }}",
            i, maskTest);
      } else {
        body += fmt::format("\n  if (({1}) && side >= 0.5) {{ tc{0}_proj.y += line_tex_offset; }}", i, maskTest);
      }
    }
    if (tcg.type == GX_TG_MTX3x4) {
      writes.emplace_back(fmt::format("tex{}", i), fmt::format("tc{}_proj", i));
    } else {
      writes.emplace_back(fmt::format("tex{}", i), fmt::format("tc{}_proj.xy", i));
    }
  }
}

// =================================================================================================
// fragment shader
// =================================================================================================
std::string Builder::kcolor_ref(int idx) {
  if (idx < 0 || idx >= static_cast<int>(MaxKColors) || m_f.kcolor[idx] < 0) {
    return "vec4(0.0)";
  }
  return fmt::format("uFS[{}]", m_f.kcolor[idx]);
}

std::string Builder::tex_size(uint32_t texMap) {
  const int at = texMap < MaxTextures ? m_f.sizeBias[texMap] : -1;
  return at >= 0 ? fmt::format("uFS[{}].xy", at) : "vec2(1.0)"s;
}

std::string Builder::sample(uint32_t texMap, const std::string& uv) {
  m_usedTextures.set(texMap);
  if (kUseTexLodBias && m_f.sizeBias[texMap] >= 0) {
    return fmt::format("texture2D(uTex{}, {}, uFS[{}].z)", texMap, uv, m_f.sizeBias[texMap]);
  }
  return fmt::format("texture2D(uTex{}, {})", texMap, uv);
}

std::string Builder::alpha_bump_sel(size_t stageIdx, const TevStage& stage) {
  if (stage.indTexStage >= m_cfg.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }
  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  default:
    Log.fatal("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  }
  // t_IndTexCoord is an exact integer in 0..255; the high bits of the mask are kept by floor(v / m) * m.
  u32 unit = 8;
  switch (stage.indTexFormat) {
  case GX_ITF_8:
    unit = 8; // mask 0xF8
    break;
  case GX_ITF_5:
    unit = 32; // mask 0xE0
    break;
  case GX_ITF_4:
    unit = 16; // mask 0xF0
    break;
  case GX_ITF_3:
    unit = 8; // mask 0xF8
    break;
  default:
    Log.fatal("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  }
  return fmt::format("(floor({0} / {1}.0) * {1}.0 / 255.0)", baseCoord, unit);
}

std::string Builder::color_arg_reg(GXTevColorArg arg, size_t stageIdx, const TevStage& stage) {
  switch (arg) {
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3(tevreg2.a)";
  case GX_CC_TEXC: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3(1.0)";
    }
    const auto& swap = m_cfg.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green),
                       chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3(1.0)";
    }
    const auto& swap = m_cfg.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC:
  case GX_CC_RASA: {
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3({})", alpha);
    }
    const u32 idx = color_channel(stage.channelId);
    const auto& swap = m_cfg.tevSwapTable[stage.tevSwapRas];
    if (arg == GX_CC_RASC) {
      return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
    }
    return fmt::format("vec3(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3(1.0)";
  case GX_CC_HALF:
    return "vec3(0.5)";
  case GX_CC_KONST: {
    switch (stage.kcSel) {
    case GX_TEV_KCSEL_8_8:
      return "vec3(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3(7.0 / 8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3(6.0 / 8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3(5.0 / 8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3(4.0 / 8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3(3.0 / 8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3(2.0 / 8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3(1.0 / 8.0)";
    case GX_TEV_KCSEL_K0:
      return kcolor_ref(0) + ".rgb";
    case GX_TEV_KCSEL_K1:
      return kcolor_ref(1) + ".rgb";
    case GX_TEV_KCSEL_K2:
      return kcolor_ref(2) + ".rgb";
    case GX_TEV_KCSEL_K3:
      return kcolor_ref(3) + ".rgb";
    case GX_TEV_KCSEL_K0_R:
      return fmt::format("vec3({}.r)", kcolor_ref(0));
    case GX_TEV_KCSEL_K1_R:
      return fmt::format("vec3({}.r)", kcolor_ref(1));
    case GX_TEV_KCSEL_K2_R:
      return fmt::format("vec3({}.r)", kcolor_ref(2));
    case GX_TEV_KCSEL_K3_R:
      return fmt::format("vec3({}.r)", kcolor_ref(3));
    case GX_TEV_KCSEL_K0_G:
      return fmt::format("vec3({}.g)", kcolor_ref(0));
    case GX_TEV_KCSEL_K1_G:
      return fmt::format("vec3({}.g)", kcolor_ref(1));
    case GX_TEV_KCSEL_K2_G:
      return fmt::format("vec3({}.g)", kcolor_ref(2));
    case GX_TEV_KCSEL_K3_G:
      return fmt::format("vec3({}.g)", kcolor_ref(3));
    case GX_TEV_KCSEL_K0_B:
      return fmt::format("vec3({}.b)", kcolor_ref(0));
    case GX_TEV_KCSEL_K1_B:
      return fmt::format("vec3({}.b)", kcolor_ref(1));
    case GX_TEV_KCSEL_K2_B:
      return fmt::format("vec3({}.b)", kcolor_ref(2));
    case GX_TEV_KCSEL_K3_B:
      return fmt::format("vec3({}.b)", kcolor_ref(3));
    case GX_TEV_KCSEL_K0_A:
      return fmt::format("vec3({}.a)", kcolor_ref(0));
    case GX_TEV_KCSEL_K1_A:
      return fmt::format("vec3({}.a)", kcolor_ref(1));
    case GX_TEV_KCSEL_K2_A:
      return fmt::format("vec3({}.a)", kcolor_ref(2));
    case GX_TEV_KCSEL_K3_A:
      return fmt::format("vec3({}.a)", kcolor_ref(3));
    default:
      Log.fatal("invalid kcSel {}", underlying(stage.kcSel));
    }
  }
  case GX_CC_ZERO:
    return "vec3(0.0)";
  default:
    Log.fatal("invalid color arg {}", underlying(arg));
  }
}

std::string Builder::alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const TevStage& stage) {
  switch (arg) {
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "1.0";
    }
    const auto& swap = m_cfg.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    const u32 idx = color_channel(stage.channelId);
    const auto& swap = m_cfg.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    switch (stage.kaSel) {
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0 / 8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0 / 8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0 / 8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0 / 8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0 / 8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0 / 8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0 / 8.0)";
    case GX_TEV_KASEL_K0_R:
      return kcolor_ref(0) + ".r";
    case GX_TEV_KASEL_K1_R:
      return kcolor_ref(1) + ".r";
    case GX_TEV_KASEL_K2_R:
      return kcolor_ref(2) + ".r";
    case GX_TEV_KASEL_K3_R:
      return kcolor_ref(3) + ".r";
    case GX_TEV_KASEL_K0_G:
      return kcolor_ref(0) + ".g";
    case GX_TEV_KASEL_K1_G:
      return kcolor_ref(1) + ".g";
    case GX_TEV_KASEL_K2_G:
      return kcolor_ref(2) + ".g";
    case GX_TEV_KASEL_K3_G:
      return kcolor_ref(3) + ".g";
    case GX_TEV_KASEL_K0_B:
      return kcolor_ref(0) + ".b";
    case GX_TEV_KASEL_K1_B:
      return kcolor_ref(1) + ".b";
    case GX_TEV_KASEL_K2_B:
      return kcolor_ref(2) + ".b";
    case GX_TEV_KASEL_K3_B:
      return kcolor_ref(3) + ".b";
    case GX_TEV_KASEL_K0_A:
      return kcolor_ref(0) + ".a";
    case GX_TEV_KASEL_K1_A:
      return kcolor_ref(1) + ".a";
    case GX_TEV_KASEL_K2_A:
      return kcolor_ref(2) + ".a";
    case GX_TEV_KASEL_K3_A:
      return kcolor_ref(3) + ".a";
    default:
      Log.fatal("invalid kaSel {}", underlying(stage.kaSel));
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  default:
    Log.fatal("invalid alpha arg {}", underlying(arg));
  }
}

bool tev_color_arg_is_normalized(GXTevColorArg arg, const std::array<bool, MaxTevRegs>& colorNormalized,
                                 const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CC_CPREV:
    return colorNormalized[GX_TEVPREV];
  case GX_CC_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CC_C0:
    return colorNormalized[GX_TEVREG0];
  case GX_CC_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CC_C1:
    return colorNormalized[GX_TEVREG1];
  case GX_CC_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CC_C2:
    return colorNormalized[GX_TEVREG2];
  case GX_CC_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

bool tev_alpha_arg_is_normalized(GXTevAlphaArg arg, const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CA_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CA_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CA_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CA_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

// One TEV stage's combine op. Colour args are vec3, alpha args float. GLSL ES 1.00 has no round()
// and no select(): the compares go through tpv_round() and ?: / mix().
std::string Builder::tev_op(GXTevOp op, std::string_view bias, std::string_view scale, std::string_view a,
                            std::string_view b, std::string_view c, std::string_view d, bool isColor) {
  m_needRound = true;
  const std::string zero = isColor ? "vec3(0.0)" : "0.0";
  switch (op) {
  case GX_TEV_ADD:
  case GX_TEV_SUB: {
    const std::string_view neg = op == GX_TEV_SUB ? "-"sv : ""sv;
    return fmt::format("(({0}mix({1}, {2}, {3}) + {4}){5}){6}", neg, a, b, c, d, bias, scale);
  }
  default:
    break;
  }
  // Compare ops: pick `c` or zero, then add `d`.
  std::string cond; // scalar bool, or bvec3 -> handled below
  bool vectorCond = false;
  const bool eq = op == GX_TEV_COMP_R8_EQ || op == GX_TEV_COMP_GR16_EQ || op == GX_TEV_COMP_BGR24_EQ ||
                  op == GX_TEV_COMP_RGB8_EQ;
  const std::string_view cmp = eq ? "=="sv : ">"sv;
  if (!isColor) {
    // The alpha unit compares the single 8-bit alpha inputs.
    cond = fmt::format("(tpv_round({0} * 255.0) {2} tpv_round({1} * 255.0))", a, b, cmp);
  } else {
    switch (op) {
    case GX_TEV_COMP_R8_GT:
    case GX_TEV_COMP_R8_EQ:
      cond = fmt::format("(tpv_round(({0}).r * 255.0) {2} tpv_round(({1}).r * 255.0))", a, b, cmp);
      break;
    case GX_TEV_COMP_GR16_GT:
    case GX_TEV_COMP_GR16_EQ:
      cond = fmt::format(
          "(tpv_round(dot(({0}).rg * 255.0, vec2(1.0, 256.0))) {2} tpv_round(dot(({1}).rg * 255.0, vec2(1.0, "
          "256.0))))",
          a, b, cmp);
      break;
    case GX_TEV_COMP_BGR24_GT:
    case GX_TEV_COMP_BGR24_EQ:
      cond = fmt::format(
          "(tpv_round(dot(({0}).rgb * 255.0, vec3(1.0, 256.0, 65536.0))) {2} tpv_round(dot(({1}).rgb * 255.0, "
          "vec3(1.0, 256.0, 65536.0))))",
          a, b, cmp);
      break;
    case GX_TEV_COMP_RGB8_GT:
    case GX_TEV_COMP_RGB8_EQ: {
      vectorCond = true;
      const auto fn = eq ? "equal" : "greaterThan";
      cond = fmt::format("{2}(tpv_round(({0}) * 255.0), tpv_round(({1}) * 255.0))", a, b, fn);
      break;
    }
    default:
      Log.fatal("unimplemented tev op {}", underlying(op));
    }
  }
  if (vectorCond) {
    return fmt::format("mix({3}, {2}, vec3({0})) + {4}", cond, ""sv, c, zero, d);
  }
  return fmt::format("({0} ? {1} : {2}) + {3}", cond, c, zero, d);
}

std::string Builder::build_fragment() {
  const auto& cfg = m_cfg;
  const auto& info = m_info;
  std::string fragmentFnPre;
  std::string fragmentFn;

  static constexpr std::array<std::string_view, 4> regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  std::array<bool, MaxTevRegs> colorNormalized{};
  std::array<bool, MaxTevRegs> alphaNormalized{};

  // --- registers ------------------------------------------------------------------------------
  if (info.loadsTevReg.test(0)) {
    fragmentFnPre += fmt::format("\n  vec4 prev = uFS[{}];", m_f.tevReg[0]);
  } else {
    fragmentFnPre += "\n  vec4 prev = vec4(0.0);";
  }
  for (uint32_t i = 1; i < MaxTevRegs; ++i) {
    if (info.loadsTevReg.test(i)) {
      fragmentFnPre += fmt::format("\n  vec4 tevreg{} = uFS[{}];", i - 1, m_f.tevReg[i]);
    } else if (info.writesTevReg.test(i)) {
      fragmentFnPre += fmt::format("\n  vec4 tevreg{} = vec4(0.0);", i - 1);
    }
  }

  // --- rasterized colours ---------------------------------------------------------------------
  for (uint32_t i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& slot = varying(fmt::format("cc{}", i));
    fragmentFnPre += fmt::format("\n  vec4 rast{} = {}{};", i, slot.var, slot.swiz);
  }

  // --- texture coordinates --------------------------------------------------------------------
  for (uint32_t i = 0; i < MaxTexCoord; ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = cfg.tcgs[i];
    const auto& slot = varying(fmt::format("tex{}", i));
    if (!is_emboss_texgen(tcg.type) && tcg.type == GX_TG_MTX3x4) {
      fragmentFnPre += fmt::format("\n  vec3 tex{0}_uvw = {1}{2};\n  vec2 tex{0}_uv = tex{0}_uvw.xy / tex{0}_uvw.z;", i,
                                   slot.var, slot.swiz);
    } else {
      fragmentFnPre += fmt::format("\n  vec2 tex{0}_uv = {1}{2};", i, slot.var, slot.swiz);
    }
  }

  // --- indirect textures ----------------------------------------------------------------------
  const auto ind_scale = [](const GXIndTexScale s) -> std::string_view {
    switch (s) {
    case GX_ITS_1:
      return "1.0"sv;
    case GX_ITS_2:
      return "(1.0 / 2.0)"sv;
    case GX_ITS_4:
      return "(1.0 / 4.0)"sv;
    case GX_ITS_8:
      return "(1.0 / 8.0)"sv;
    case GX_ITS_16:
      return "(1.0 / 16.0)"sv;
    case GX_ITS_32:
      return "(1.0 / 32.0)"sv;
    case GX_ITS_64:
      return "(1.0 / 64.0)"sv;
    case GX_ITS_128:
      return "(1.0 / 128.0)"sv;
    case GX_ITS_256:
      return "(1.0 / 256.0)"sv;
    default:
      Log.fatal("unhandled indirect scale {}", underlying(s));
    }
  };
  const auto tex_scale_ref = [&](uint32_t texCoord) {
    return m_f.texScale[texCoord] >= 0 ? fmt::format("uFS[{}].xy", m_f.texScale[texCoord]) : "vec2(1.0)"s;
  };
  for (uint32_t i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = cfg.indStages[i];
    const u32 texCoordId = underlying(indStage.texCoordId);
    const u32 texMapId = underlying(indStage.texMapId);
    // GX applies the SU texture-coordinate scale before the indirect stage scale. The shader carries
    // normalized UVs, so convert that texel-space result back into normalized coordinates.
    const auto scaleExpr = fmt::format("tex{0}_uv * {1} * vec2({2}, {3}) / {4}", texCoordId, tex_scale_ref(texCoordId),
                                       ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), tex_size(texMapId));
    // Rounded to the exact 0..255 integer (upstream truncates through u32()).
    m_needRound = true;
    fragmentFnPre += fmt::format(
        "\n  // Indirect stage {0}"
        "\n  vec3 t_IndTexCoord{0} = tpv_round(255.0 * {1}.abg);",
        i, sample(texMapId, scaleExpr));
  }
  if (info.usedIndStages.any()) {
    fragmentFnPre += "\n  vec2 t_TexCoord = vec2(0.0);";
  }
  for (uint32_t i = 0; i < cfg.tevStageCount; ++i) {
    const auto& stage = cfg.tevStages[i];
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < cfg.numIndStages;
    const bool needsTevTexCoord =
        needsIndirectCoord || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    const bool needsTextureSample = uses_texture_sample(stage);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseTexCoord = stage.texCoordId != GX_TEXCOORD_NULL;
    const bool hasBaseTexture = stage.texMapId != GX_TEXMAP_NULL;
    const bool hasBaseCoord = hasBaseTexCoord && hasBaseTexture;
    std::string uvIn;
    if (needsTevTexCoord) {
      fragmentFnPre += fmt::format("\n  // TEV stage {} indirect", i);

      // Apply indirect texture matrix (produces a texel-space offset)
      std::string indirectOffsetTexel;
      if (needsIndirectCoord && hasIndirectStage) {
        std::string_view fmtShift;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtShift = " / 8.0"sv;
          break;
        case GX_ITF_4:
          fmtShift = " / 16.0"sv;
          break;
        case GX_ITF_3:
          fmtShift = " / 32.0"sv;
          break;
        default:
          Log.fatal("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtShift.empty()) {
          fragmentFnPre += fmt::format("\n  vec3 ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fragmentFnPre += fmt::format("\n  vec3 ind{0}_coord = floor(t_IndTexCoord{1}{2});", i,
                                       underlying(stage.indTexStage), fmtShift);
        }

        if (stage.indTexBiasSel != GX_ITB_NONE) {
          const auto bias = stage.indTexFormat == GX_ITF_8 ? "-128.0"sv : "1.0"sv;
          auto biasS = "0.0"sv, biasT = "0.0"sv, biasU = "0.0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1.0"sv;
          }
          fragmentFnPre += fmt::format("\n  ind{0}_coord = ind{0}_coord + vec3({1}, {2}, {3}) * {4};", i, biasS, biasT,
                                       biasU, bias);
        }

        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          // Static 2x3 matrix: dot(mat_row, vec3(S,T,U)) * scale
          const u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          const int at = m_f.indMtx[mtxIdx];
          fragmentFnPre += fmt::format(
              "\n  vec4 ind{0}_c0 = uFS[{1}];"
              "\n  vec4 ind{0}_c1 = uFS[{2}];",
              i, at, at + 1);
          indirectOffsetTexel = fmt::format(
              "vec2("
              "dot(vec3(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "
              "dot(vec3(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)"
              ") * ind{0}_c1.z",
              i);
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          // Dynamic S: result = scaled texcoord * ind_coord.x * scale / 256
          const u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          const u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format("tex{1}_uv * {3} * ind{0}_coord.x * uFS[{2}].z / 256.0", i, regTexCoord,
                                            m_f.indMtx[mtxIdx] + 1, tex_scale_ref(regTexCoord));
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          // Dynamic T: result = scaled texcoord * ind_coord.y * scale / 256
          const u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          const u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format("tex{1}_uv * {3} * ind{0}_coord.y * uFS[{2}].z / 256.0", i, regTexCoord,
                                            m_f.indMtx[mtxIdx] + 1, tex_scale_ref(regTexCoord));
        }
      }

      // Don't convert to/from texel space if we can avoid it
      const bool useSimpleCoords = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;

      // Wrap base coord and combine with the indirect translation. GLSL's mod() is floored; the
      // WGSL `%` truncates, so keep that behaviour.
      const auto wrap_comp = [this](GXIndTexWrap wrap, const std::string& coord) -> std::string {
        std::string_view size;
        switch (wrap) {
        case GX_ITW_OFF:
          return coord;
        case GX_ITW_256:
          size = "256.0"sv;
          break;
        case GX_ITW_128:
          size = "128.0"sv;
          break;
        case GX_ITW_64:
          size = "64.0"sv;
          break;
        case GX_ITW_32:
          size = "32.0"sv;
          break;
        case GX_ITW_16:
          size = "16.0"sv;
          break;
        case GX_ITW_0:
          return "0.0";
        default:
          Log.fatal("unhandled indirect wrap {}", underlying(wrap));
        }
        m_needFmod = true;
        return fmt::format("tpv_fmod({}, {})", coord, size);
      };
      std::string baseCoordExpr;
      if (hasBaseCoord) {
        const u32 texCoordId = underlying(stage.texCoordId);
        if (useSimpleCoords) {
          baseCoordExpr = fmt::format("tex{}_uv", texCoordId);
        } else {
          fragmentFnPre += fmt::format("\n  vec2 ind{0}_texel = tex{1}_uv * {2};", i, texCoordId,
                                       tex_scale_ref(texCoordId));
          baseCoordExpr = fmt::format("ind{}_texel", i);
        }
      }
      std::string wrappedExpr = baseCoordExpr;
      if (!baseCoordExpr.empty() && (stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF)) {
        wrappedExpr = fmt::format("vec2({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                                  wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      }

      std::string finalCoord;
      if (!wrappedExpr.empty() && !indirectOffsetTexel.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetTexel);
      } else if (!wrappedExpr.empty()) {
        finalCoord = wrappedExpr;
      } else {
        finalCoord = indirectOffsetTexel;
      }

      if (info.usedIndStages.any() && !finalCoord.empty()) {
        if (stage.indTexAddPrev) {
          fragmentFnPre += fmt::format("\n  t_TexCoord += {};", finalCoord);
        } else {
          fragmentFnPre += fmt::format("\n  t_TexCoord = {};", finalCoord);
        }

        if (needsTextureSample && hasBaseTexture) {
          const u32 texMapId = underlying(stage.texMapId);
          if (useSimpleCoords) {
            fragmentFnPre += fmt::format("\n  vec2 ind{0}_uv = t_TexCoord;", i);
          } else {
            fragmentFnPre += fmt::format("\n  vec2 ind{0}_uv = t_TexCoord / {1};", i, tex_size(texMapId));
          }
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!needsTextureSample) {
      continue;
    }
    if (stage.texMapId == GX_TEXMAP_NULL || stage.texCoordId == GX_TEXCOORD_NULL) {
      Log.fatal("unmapped texture / texcoord for stage {}", i);
    }
    if (uvIn.empty()) {
      uvIn = fmt::format("tex{0}_uv", underlying(stage.texCoordId));
    }
    fragmentFnPre += fmt::format("\n  vec4 sampled{0} = {1};", i, sample(underlying(stage.texMapId), uvIn));
  }

  // --- TEV stages -----------------------------------------------------------------------------
  for (uint32_t idx = 0; idx < cfg.tevStageCount; ++idx) {
    const auto& stage = cfg.tevStages[idx];
    {
      const auto color_arg = [&](GXTevColorArg arg) {
        auto value = color_arg_reg(arg, idx, stage);
        if (tev_color_arg_is_normalized(arg, colorNormalized, alphaNormalized)) {
          return fmt::format("vec3({})", value);
        }
        m_needOverflow = true;
        return fmt::format("tpv_overflow({})", value);
      };
      const auto outReg = regName[stage.colorOp.outReg];
      const auto op = tev_op(stage.colorOp.op, tev_bias(stage.colorOp.bias), tev_scale(stage.colorOp.scale),
                             color_arg(stage.colorPass.a), color_arg(stage.colorPass.b), color_arg(stage.colorPass.c),
                             color_arg_reg(stage.colorPass.d, idx, stage), true);
      const auto clamped = stage.colorOp.clamp ? fmt::format("clamp({}, vec3(0.0), vec3(1.0))", op)
                                               : fmt::format("clamp({}, vec3(-4.0), vec3(4.0))", op);
      fragmentFn += fmt::format("\n  // TEV stage {2}\n  {0} = vec4({1}, {0}.a);", outReg, clamped, idx);
      colorNormalized[stage.colorOp.outReg] = stage.colorOp.clamp;
    }
    {
      const auto alpha_arg = [&](GXTevAlphaArg arg) {
        auto value = alpha_arg_reg(arg, idx, stage);
        if (tev_alpha_arg_is_normalized(arg, alphaNormalized)) {
          return value;
        }
        m_needOverflow = true;
        return fmt::format("tpv_overflow({})", value);
      };
      const auto outReg = regName[stage.alphaOp.outReg];
      const auto op = tev_op(stage.alphaOp.op, tev_bias(stage.alphaOp.bias), tev_scale(stage.alphaOp.scale),
                             alpha_arg(stage.alphaPass.a), alpha_arg(stage.alphaPass.b), alpha_arg(stage.alphaPass.c),
                             alpha_arg_reg(stage.alphaPass.d, idx, stage), false);
      const auto clamped = stage.alphaOp.clamp ? fmt::format("clamp({}, 0.0, 1.0)", op)
                                               : fmt::format("clamp({}, -4.0, 4.0)", op);
      fragmentFn += fmt::format("\n  {0}.a = {1};", outReg, clamped);
      alphaNormalized[stage.alphaOp.outReg] = stage.alphaOp.clamp;
    }
  }

  bool prevColorNormalized = true;
  bool prevAlphaNormalized = true;
  if (cfg.tevStageCount > 0) {
    const auto& lastStage = cfg.tevStages[cfg.tevStageCount - 1];
    prevColorNormalized = colorNormalized[lastStage.colorOp.outReg];
    prevAlphaNormalized = alphaNormalized[lastStage.alphaOp.outReg];
    if (lastStage.colorOp.outReg != 0) {
      fragmentFn += fmt::format("\n  prev = vec4({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
    }
    if (lastStage.alphaOp.outReg != 0) {
      fragmentFn += fmt::format("\n  prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
    }
  }

  // --- fog ------------------------------------------------------------------------------------
  if (info.usesFog) {
    const int fog = m_f.fog;
    // fog.a/b/c live in the second vec4 (x, y, z); the colour is the first.
    if ((cfg.fogType & 0x08) != 0) {
      fragmentFn += fmt::format("\n  // Orthographic fog\n  float fogBase = uFS[{}].x * gl_FragCoord.z;", fog + 1);
    } else {
      fragmentFn += fmt::format("\n  // Perspective fog\n  float fogBase = uFS[{}].x / (uFS[{}].y - gl_FragCoord.z);",
                                fog + 1, fog + 1);
    }
    if (cfg.fogRangeEnabled) {
      // Fog range adjustment: a per-column table of scale factors, index by the output pixel X.
      // The table was built into frame.storage (draw.immediateData.fogRangeBase) and is exposed as a
      // 1-row RGBA8 texture (16-bit encoding, see fog_range_encode), so GLSL ES 1.00 can index it with
      // gl_FragCoord.x (no dynamic array index). gl_FragCoord.x is already the pixel centre (x + 0.5) and
      // uFogRangeSize.x = 1 / targetWidth, so the coordinate hits the middle of texel x (NEAREST).
      fragmentFn += fmt::format("\n  float fogRangeScale = {};", FogRangeSampleGlsl);
      // Upstream multiplies fogBase by the LUT factor (shader.cpp: fogBase *= abuf[...]).
      fragmentFn += "\n  fogBase *= fogRangeScale;";
    }
    fragmentFn += fmt::format("\n  float fogF = clamp(fogBase - uFS[{}].z, 0.0, 1.0);", fog + 1);
    switch (cfg.fogType) {
    case GX_FOG_PERSP_LIN:
    case GX_FOG_ORTHO_LIN:
      fragmentFn += "\n  float fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fragmentFn += "\n  float fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fragmentFn += "\n  float fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fragmentFn += "\n  float fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fragmentFn +=
          "\n  fogF = 1.0 - fogF;"
          "\n  float fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    default:
      Log.fatal("invalid fog type {}", cfg.fogType);
    }
    fragmentFn +=
        fmt::format("\n  prev = vec4(mix(prev.rgb, uFS[{}].rgb, clamp(fogZ, 0.0, 1.0)), prev.a);", fog);
  }

  if (!prevColorNormalized && !prevAlphaNormalized) {
    m_needOverflow = true;
    fragmentFn += "\n  prev = tpv_overflow(prev);";
  } else if (!prevColorNormalized) {
    m_needOverflow = true;
    fragmentFn += "\n  prev = vec4(tpv_overflow(prev.rgb), prev.a);";
  } else if (!prevAlphaNormalized) {
    m_needOverflow = true;
    fragmentFn += "\n  prev.a = tpv_overflow(prev.a);";
  }

  // --- alpha compare --------------------------------------------------------------------------
  if (cfg.alphaCompare) {
    const auto comp0 = alpha_compare(cfg.alphaCompare.comp0, cfg.alphaCompare.ref0);
    const auto comp1 = alpha_compare(cfg.alphaCompare.comp1, cfg.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (cfg.alphaCompare.op) {
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    default:
      Log.fatal("invalid alpha compare op {}", underlying(cfg.alphaCompare.op));
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fragmentFn += "\n  // Alpha compare\n  discard;";
    } else if (discard.constant != 0) {
      m_needRound = true;
      fragmentFn +=
          "\n  // Alpha compare"
          "\n  float aref = tpv_round(clamp(prev.a, 0.0, 1.0) * 255.0);";
      fragmentFn += fmt::format("\n  if ({}) {{ discard; }}", discard.expr);
    }
  }

  // --- output ---------------------------------------------------------------------------------
  if (m_replaceAlpha) {
    // GX destination alpha: the constant replaces the TEV alpha once alpha compare has run.
    fragmentFn += "\n  gl_FragColor = vec4(prev.rgb, uExtra.x);";
  } else {
    fragmentFn += "\n  gl_FragColor = prev;";
  }

  // --- assemble -------------------------------------------------------------------------------
  std::string src;
  src.reserve(8192);
  src += "// GX fragment shader (generated by vita_shader.cpp)\n";
  src += fmt::format("precision {} float;\n", kFragmentPrecision);
  if (m_fs.size != 0) {
    src += fmt::format("uniform vec4 uFS[{}];\n", m_fs.size);
  }
  if (m_replaceAlpha) {
    src += "uniform vec4 uExtra;\n";
  }
  for (uint32_t i = 0; i < MaxTextures; ++i) {
    if (m_usedTextures.test(i)) {
      src += fmt::format("uniform sampler2D uTex{};\n", i);
    }
  }
  if (m_cfg.fogRangeEnabled) {
    src += "uniform sampler2D uFogRange;\n";
    src += "uniform vec2 uFogRangeSize;\n";
    src += FogRangeDecodeGlsl;
  }
  if (!m_packed) {
    for (const auto& [name, slot] : m_varySlots) {
      src += fmt::format("varying {} {};\n", slot.comps == 4 ? "vec4" : slot.comps == 3 ? "vec3" : "vec2", slot.var);
    }
  } else {
    for (uint32_t s = 0; s < m_varyVec4; ++s) {
      src += fmt::format("varying vec4 vP{};\n", s);
    }
  }
  if (m_needRound) {
    src +=
        "float tpv_round(float x) { return floor(x + 0.5); }\n"
        "vec3 tpv_round(vec3 x) { return floor(x + 0.5); }\n";
  }
  if (m_needOverflow) {
    // TEV values that left [0,1] wrap in 8-bit space.
    src +=
        "float tpv_overflow(float v) { float b = v * 255.0; return (b - floor(b / 256.0) * 256.0) / 255.0; }\n"
        "vec3 tpv_overflow(vec3 v) { vec3 b = v * 255.0; return (b - floor(b / 256.0) * 256.0) / 255.0; }\n"
        "vec4 tpv_overflow(vec4 v) { vec4 b = v * 255.0; return (b - floor(b / 256.0) * 256.0) / 255.0; }\n";
  }
  if (m_needFmod) {
    src += "float tpv_fmod(float x, float y) { return x - y * (x < 0.0 ? ceil(x / y) : floor(x / y)); }\n";
  }
  src += "void main() {";
  src += fragmentFnPre;
  src += fragmentFn;
  src += "\n}\n";
  return src;
}

ProgramSource Builder::build() {
  analyze();
  allocate_uniforms();
  plan_varyings();

  ProgramSource ps;
  ps.layout = m_layout;
  // The fragment shader is generated first because its sampler/varying use feeds the declarations;
  // the vertex shader is independent of it except for the shared varying plan.
  ps.vertex = build_vertex();
  ps.fragment = build_fragment();
  ps.vsVec4 = m_vs.size;
  ps.fsVec4 = m_fs.size;
  ps.vsSegs = std::move(m_vs.segs);
  ps.fsSegs = std::move(m_fs.segs);
  ps.usesExtra = m_replaceAlpha;
  ps.usesFogRange = m_cfg.fogRangeEnabled;
  ps.textures = m_usedTextures;

  // Bringup guard: a stage array larger than vitaGL's limit fails at glLinkProgram with no usable
  // error on NO_DEBUG builds. 16 fragment vec4s is the tight one; warn so a heavy TEV/indirect
  // config is noticed before it is blamed on a missing libshacccg.suprx. (Avoided during normal
  // runs because every real draw stays well below the caps.)
  if (ps.fsVec4 > kMaxFragmentUniformVec4) {
    Log.warn("fragment shader uses {} vec4 uniforms (vitaGL limit {}) - link will fail; config={}",
             ps.fsVec4, kMaxFragmentUniformVec4, static_cast<unsigned>(m_cfg.tevStageCount));
  }
  if (ps.vsVec4 > kMaxVertexUniformVec4) {
    Log.warn("vertex shader uses {} vec4 uniforms (vitaGL limit {}) - link will fail", ps.vsVec4,
             kMaxVertexUniformVec4);
  }
  return ps;
}
} // namespace

ProgramSource build_program_source(const PipelineConfig& config, const ShaderInfo& info, bool pnMtxZero,
                                   uint32_t lights) {
  Builder builder(config, info, pnMtxZero, lights);
  return builder.build();
}

LightMaskLocation light_mask_location(const ShaderConfig& config, const ShaderInfo& info) {
  LightMaskLocation loc;
  if (!info.lightingEnabled) {
    return loc;
  }
  // Mirrors Builder::analyze (m_chanLit) and build_vertex (which channels get lighting_func): only sampled colour
  // channels are shaded, and lane k of the mask vec4 is channel k (COLOR0, COLOR1, ALPHA0, ALPHA1).
  for (uint32_t i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    if (config.colorChannels[i].lightingEnabled) {
      loc.litChannels |= static_cast<uint8_t>(1u << i);
    }
    if (config.colorChannels[i + GX_ALPHA0].lightingEnabled) {
      loc.litChannels |= static_cast<uint8_t>(1u << (i + GX_ALPHA0));
    }
  }
  if (loc.litChannels != 0) {
    loc.vec4 = make_blob_layout(config, info).lightState;
  }
  return loc;
}

} // namespace aurora::gx::vita
