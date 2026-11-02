// Contract between the pieces of the Vita GX renderer. Each piece lives in its own file:
//
//   gx/vita_vertex.cpp   decoded_layout(), decode_draw()   GX vertex data -> GL vertex buffer
//   gx/vita_shader.cpp   build_program_source()            ShaderConfig -> GLSL ES 1.00 programs
//   gx/texture.cpp, gfx/vita_texture.cpp                   GX texture decode + GL texture objects
//   gfx/vita_gl_replay.cpp                                 GL state, programs, EFB, present
//
// Nothing in here may include GL headers or WebGPU.
#pragma once

#include "gx.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"
#include "../gfx/vita_frame.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace aurora::gx::vita {

// ---- decoded vertex layout --------------------------------------------------------------------
// GLES2 cannot pull vertices from storage buffers and GXM cannot byte-swap, so the CPU decodes every
// GX vertex of a draw into an interleaved buffer whose layout depends only on the ShaderConfig.
// One attribute per GX vertex attribute that is present (ShaderConfig::attrs[i].attrType != GX_NONE):
//
//   GX attribute            decoded as                          GLSL attribute
//   GX_VA_PNMTXIDX          1 float  (matrix index 0..9)        float aPnMtxIdx
//   GX_VA_TEXnMTXIDX        1 float  (n = 0..7)                 float aTexMtxIdx<n>
//   GX_VA_POS               3 floats (z = 0 for 2-component)    vec3  aPos
//   GX_VA_NRM               3 floats                            vec3  aNrm
//   GX_VA_NRM with NBT3     9 floats: normal, binormal, tangent vec3  aNrm, aBin, aTan   (3 entries)
//   GX_VA_CLR0 / CLR1       4 unsigned bytes, normalized RGBA   vec4  aClr0 / aClr1
//   GX_VA_TEXn              2 floats (t = 0 for 1-component)    vec2  aTex<n>
//
// Entries are in this table's order (NBT3 expands in place); every offset is a multiple of 4.
//
// aPnMtxIdx is ALWAYS present: when the vertex format has no per-vertex matrix index the decoder
// fills it from draw.immediateData.currentPnMtx (what upstream reads from `imm.current_pnmtx`).
//
// Line / point draws (ShaderConfig::lineMode != 0) are expanded on the CPU into one quad (4 decoded
// vertices, 6 indices) per segment/point, so the vertex shader keeps upstream's screen-space
// expansion math. These extra entries (attr == GX_VA_NULL) are appended after the table above:
//
//   aCorner        1 float   0..3, the `vidx` of upstream's shader. Corners 0,1 belong to endpoint A
//                            and 2,3 to endpoint B; (corner & 1) selects the side of the line.
//   aPosOther      3 floats  lineMode 1/2 only: position of the segment's *other* endpoint
//   aPnMtxIdxOther 1 float   lineMode 1/2 only: matrix index of the other endpoint
//
// A vertex's own attributes (aPos, aPnMtxIdx, tex coords, colors, ...) come from its own endpoint,
// exactly like upstream (which loads every attribute through the selected endpoint index; only
// position/matrix index of both endpoints are needed for the direction).
// Content-addressed vertex decode cache (gx/vita_vertex.cpp). Keyed on the source bytes and the decoded
// layout, never on recorded offsets, because Frame::storage is rebuilt every frame.
void vertex_cache_clear() noexcept;
// Logs hit/miss/eviction counters and the bytes held. Temporary diagnostic; remove with the probe set.
void vertex_cache_report(const char* label) noexcept;

struct DecodedAttr {
  enum class Kind : uint8_t { Float, UByte4Norm };
  GXAttr attr;        // GX_VA_*
  Kind kind;
  uint8_t components; // Float: 1..3, UByte4Norm: 4
  uint16_t offset;    // byte offset inside the decoded vertex
  const char* name;   // GLSL attribute name (string literal)
};
constexpr size_t MaxDecodedAttrs = 32;
struct DecodedLayout {
  std::array<DecodedAttr, MaxDecodedAttrs> attrs{};
  uint8_t count = 0;
  uint16_t stride = 0; // bytes per decoded vertex (multiple of 4)
};
DecodedLayout decoded_layout(const ShaderConfig& config) noexcept; // vita_vertex.cpp

// Decodes draw.vtxCount GX vertices of `draw` into `out` (resized to vtxCount * layout.stride).
// Raw vertex bytes are frame.verts[draw.vertRange.offset ...]; indexed attributes read
// frame.storage[draw.immediateData.arrayStart[attr - GX_VA_POS] + index * stride].
// Line and point primitives (shaderConfig.lineMode != 0) are NOT handled here; see expand_lines().
void decode_draw(const PipelineConfig& config, const DrawData& draw, const gfx::vita::Frame& frame,
                 const DecodedLayout& layout, std::vector<uint8_t>& out); // vita_vertex.cpp

// ---- fog range table ----------------------------------------------------------------------------
// The command processor builds one factor per render-target column: sqrt(offset^2 + k^2) / k, which is
// >= 1 everywhere (1 at the range centre, growing towards the edges, huge for tiny k). RGBA8 cannot hold
// that directly, so the replay stores it as a 16-bit fixed-point value over [0, FogRangeMax] in R (high
// byte) and G (low byte); larger factors saturate (fog is already fully opaque by then). The GLSL decoder
// below must stay in sync with fog_range_encode().
constexpr float FogRangeMax = 32.f;
inline void fog_range_encode(float factor, uint8_t& hi, uint8_t& lo) noexcept {
  const float clamped = factor > 0.f ? (factor < FogRangeMax ? factor : FogRangeMax) : 0.f; // also maps NaN to 0
  const auto n = static_cast<uint32_t>(clamped * (65535.f / FogRangeMax) + 0.5f);
  hi = static_cast<uint8_t>(n >> 8);
  lo = static_cast<uint8_t>(n & 0xFF);
}
inline float fog_range_decode(uint8_t hi, uint8_t lo) noexcept {
  return static_cast<float>(hi * 256u + lo) * (FogRangeMax / 65535.f);
}
// GLSL ES 1.00 counterpart of fog_range_decode(); emitted into every fragment shader that samples uFogRange.
inline constexpr const char* FogRangeDecodeGlsl =
    "float tpv_fog_range(vec4 t) {\n"
    "  return (floor(t.r * 255.0 + 0.5) * 256.0 + floor(t.g * 255.0 + 0.5)) * (32.0 / 65535.0);\n"
    "}\n";
// The sampling expression the generator uses (and the host test, so it cannot drift): gl_FragCoord.x is the
// pixel CENTRE (x + 0.5) in window coordinates, so x * (1 / width) already hits the middle of texel x.
inline constexpr const char* FogRangeSampleGlsl =
    "tpv_fog_range(texture2D(uFogRange, vec2(gl_FragCoord.x * uFogRangeSize.x, 0.5)))";

// ---- generated GL program ---------------------------------------------------------------------
// The uniform blob a draw carries (frame.uniforms[draw.uniformRange]) is what gx::build_uniform
// produced, a sequence of vec4s in the exact order of fill_uniform() in shader_info.cpp. A GLES2
// program has per-stage uniform arrays instead, so the generator reports which vec4 ranges of the
// blob each stage reads and where they land in that stage's array.
//
// A few blob lanes are not floats: build_uniform appends the light masks and the point/line texcoord mask
// as raw u32 bit fields (WGSL reads them as u32). GLSL ES 1.00 has no floatBitsToUint, and a small integer
// read as a float bit pattern is a denormal (= 0), so such lanes are converted to their numeric float value
// at upload (`intLanes`) and the shader tests bits arithmetically with mod() / floor().
struct UniformSeg {
  uint16_t src;   // first vec4 in the blob
  uint16_t dst;   // first vec4 in the stage array
  uint16_t count; // vec4s
  uint8_t intLanes = 0; // bit n set: lane n of every vec4 in the segment holds a u32 (exact below 2^24)
};
// Rewrites the u32 lanes selected by `intLanes` of `vec4s` consecutive vec4s (already copied from the blob) in
// place to the equal-valued floats.
inline void convert_int_lanes(float* data, uint32_t vec4s, uint8_t intLanes) {
  if (intLanes == 0) {
    return;
  }
  for (uint32_t v = 0; v < vec4s; ++v) {
    for (uint32_t lane = 0; lane < 4; ++lane) {
      if ((intLanes >> lane) & 1u) {
        uint32_t bits;
        std::memcpy(&bits, data + v * 4 + lane, sizeof(bits));
        data[v * 4 + lane] = static_cast<float>(bits);
      }
    }
  }
}
struct ProgramSource {
  std::string vertex;   // GLSL ES 1.00
  std::string fragment; // GLSL ES 1.00
  // Stage uniform arrays: `uniform vec4 uVS[vsVec4];` / `uniform vec4 uFS[fsVec4];` (omitted when 0).
  uint16_t vsVec4 = 0;
  uint16_t fsVec4 = 0;
  std::vector<UniformSeg> vsSegs;
  std::vector<UniformSeg> fsSegs;
  // Per-draw scalar constants: `uniform vec4 uExtra;` in the fragment shader (omitted if unused).
  //   uExtra.x = DrawData::dstAlpha / 255.0 (only meaningful when the config replaces dest alpha)
  bool usesExtra = false;
  // Fog range adjustment: the fragment shader samples a 1-row RGBA8 texture (uFogRange, unit 15) holding
  // one scale factor per output pixel column, encoded with fog_range_encode(); uFogRangeSize.x = 1 / width
  // of the render target the draw goes to.
  bool usesFogRange = false;
  // `uniform sampler2D uTex<i>;` exists for every set bit i; the program samples texture unit i.
  std::bitset<MaxTextures> textures;
  DecodedLayout layout; // == decoded_layout(config.shaderConfig)
};
// True when every vertex of a draw with this config and `currentPnMtx` reads position/normal matrix 0:
// the vertex format has no per-vertex matrix index AND the draw's GXSetCurrentMtx value (which the
// decoder uses for aPnMtxIdx, see above) is 0. Only then may the program carry a single matrix bank.
// The game does use other values (d_home_button.cpp: GXSetCurrentMtx(3) = GX_PNMTX1).
inline bool pn_mtx_provably_zero(const ShaderConfig& config, uint32_t currentPnMtx) noexcept {
  return config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE && currentPnMtx == 0;
}

// ---- light slots ------------------------------------------------------------------------------
// A lit shader reads the light objects (5 vec4 each) of the lights its channel masks can enable. The masks are
// draw state (u32 lanes in the uniform blob), not part of the ShaderConfig, so by default a program carries all
// GX::MaxLights = 8 slots (40 vec4). A draw rarely enables more than a few: the replay reads the masks of the
// draw, picks the smallest bucket that covers the highest enabled light and asks the generator for a program with
// that many slots. This is what keeps a skinned + lit + dynamic-texture-matrix draw (138 vec4 with 8 slots) under
// vitaGL's 128-vec4 vertex cap: 6 slots is exactly 128, fewer is below.
constexpr uint32_t MaxLightSlots = GX::MaxLights;
// Smallest allowed slot count (1, 2, 4, 6 or 8) that holds `needed` lights; 0 needed still gets 1.
constexpr uint32_t light_bucket(uint32_t needed) noexcept {
  return needed <= 1 ? 1u : needed <= 2 ? 2u : needed <= 4 ? 4u : needed <= 6 ? 6u : MaxLightSlots;
}
// Index of a bucket value (0..4), for packing it into a cache key.
constexpr uint32_t light_bucket_index(uint32_t bucket) noexcept {
  return bucket <= 1 ? 0u : bucket <= 2 ? 1u : bucket <= 4 ? 2u : bucket <= 6 ? 3u : 4u;
}
// Where the light masks of a draw live in its uniform blob. Depends only on the shader config, so callers cache it
// per config. vec4 < 0: the program does not read light masks (unlit, or emboss-only lights).
struct LightMaskLocation {
  int32_t vec4 = -1;       // blob position (in vec4s) of the 4 mask lanes: COLOR0, COLOR1, ALPHA0, ALPHA1 (u32)
  uint8_t litChannels = 0; // bit k set: lane k belongs to a channel the shader actually lights
};
LightMaskLocation light_mask_location(const ShaderConfig& config, const ShaderInfo& info); // vita_shader.cpp
// Slot count a draw needs, from its uniform blob. Returns MaxLightSlots (the canonical program) when the program does
// not depend on the masks or the blob is too short to hold them.
inline uint32_t lights_in_use(const LightMaskLocation& loc, const void* blob, size_t blobBytes) noexcept {
  if (loc.vec4 < 0 || loc.litChannels == 0 || static_cast<size_t>(loc.vec4 + 1) * 16 > blobBytes) {
    return MaxLightSlots;
  }
  uint32_t mask = 0;
  for (uint32_t lane = 0; lane < 4; ++lane) {
    if ((loc.litChannels >> lane) & 1u) {
      uint32_t v;
      std::memcpy(&v, static_cast<const uint8_t*>(blob) + static_cast<size_t>(loc.vec4) * 16 + lane * 4, sizeof(v));
      mask |= v;
    }
  }
  mask &= (1u << MaxLightSlots) - 1u;
  uint32_t needed = 0;
  while ((mask >> needed) != 0) {
    ++needed; // highest set bit + 1
  }
  return light_bucket(needed);
}

// Deterministic in its inputs. `info` is build_shader_info(config.shaderConfig).
// `pnMtxZero` (see pn_mtx_provably_zero) lets the generator upload one matrix bank instead of all
// MaxPnMtx; `lights` (1..MaxLightSlots, see light_bucket) limits the light slots a lit program reads. Both are part
// of the program's identity, so callers must key their program cache on them. A program with `lights` slots
// silently ignores mask bits >= lights, so the caller must pick it from the draw's own masks (lights_in_use);
// emboss texgens that name a higher light raise the count themselves. The defaults are the safe full programs.
ProgramSource build_program_source(const PipelineConfig& config, const ShaderInfo& info, bool pnMtxZero = false,
                                   uint32_t lights = MaxLightSlots); // vita_shader.cpp

// ---- line / point expansion -------------------------------------------------------------------
// For shaderConfig.lineMode != 0 (draw.instanceCount segments/points, draw.vtxCount raw vertices:
// LINES = pairs, LINESTRIP = consecutive pairs, POINTS = every vertex) the decoder emits triangle
// lists with the extra attributes documented above. Each chunk has at most 49152 vertices and indexes
// its own vertices from 0; quad corner order is (A,-),(A,+),(B,-),(B,+) -> indices 0,1,2, 2,1,3.
// Points: corners 0..3 of one vertex, corner bit0 = x sign, bit1 = y sign (see upstream shader).
struct ExpandedPrimitives {
  std::vector<uint8_t> vertices; // decoded vertices, layout.stride each
  std::vector<uint16_t> indices;
};
void expand_lines(const PipelineConfig& config, const DrawData& draw, const gfx::vita::Frame& frame,
                  const DecodedLayout& layout, std::vector<ExpandedPrimitives>& chunks); // vita_vertex.cpp

} // namespace aurora::gx::vita
