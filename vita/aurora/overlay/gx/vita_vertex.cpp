// Vita backend: CPU decode of GX vertex data into the interleaved layout the GLES2 shaders read.
//
// Upstream Aurora pulls raw big-endian vertex bytes out of storage buffers in the vertex shader
// (see attr_load()/fetch_*() in gx/shader.cpp). GLES2/GXM have neither storage buffers nor byte
// swapping vertex fetch, so the work moves here: every GX vertex of a draw is decoded once, on the CPU,
// into floats / normalized ubytes laid out by decoded_layout(). The semantics follow upstream's WGSL
// exactly (direct vs INDEX8/INDEX16 addressing, per-array endianness, fixed-point `frac` scaling,
// the /3 on PNMTXIDX, NBT slices, color format expansion).
//
// Speed: the decoder works attribute-major. For every attribute of the layout one specialized tight loop
// (template instantiation per source mode x component type x counts) walks all vertices of the draw, so
// no per-vertex format branching remains.
#include "vita_gx.hpp"
#include "../logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "vertex decoder assumes a little-endian host");

namespace aurora::gx::vita {
namespace {

constexpr Module Log{"aurora::gx::vita::vertex"};

constexpr uint32_t MaxChunkVertices = 49152;

// ---- little helpers ---------------------------------------------------------------------------

inline uint16_t ld16(const uint8_t* p) noexcept {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t ld32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline void st32(uint8_t* p, uint32_t v) noexcept { std::memcpy(p, &v, sizeof(v)); }
inline void stf(uint8_t* p, float v) noexcept { std::memcpy(p, &v, sizeof(v)); }

// Reads a 16-bit value; SWAP = the stored bytes are big-endian (the host is little-endian).
template <bool SWAP>
inline uint16_t rd16(const uint8_t* p) noexcept {
  const uint16_t v = ld16(p);
  return SWAP ? __builtin_bswap16(v) : v;
}

// ---- color expansion tables (round(v * 255 / max)) --------------------------------------------

template <uint32_t MAX>
struct ColorTable {
  uint8_t v[MAX + 1];
  constexpr ColorTable() : v() {
    for (uint32_t i = 0; i <= MAX; ++i) {
      v[i] = static_cast<uint8_t>((2 * i * 255 + MAX) / (2 * MAX));
    }
  }
};
constexpr ColorTable<31> Tab5;
constexpr ColorTable<63> Tab6;
constexpr ColorTable<15> Tab4;

// ---- decode plans -----------------------------------------------------------------------------

enum class PlanKind : uint8_t { Zero, Const, MtxIdx, Float, Color };
enum class ColorFmt : uint8_t { Rgb565, Rgb8, Rgbx8, Rgba4, Rgba6, Rgba8 };
enum SrcMode : int { Direct = 0, Idx8 = 1, Idx16 = 2 };

struct Plan {
  PlanKind kind = PlanKind::Zero;
  uint8_t mode = Direct;
  uint8_t compType = 0; // GXCompType for Float
  uint8_t inCnt = 0;    // Float: components in the GX data
  uint8_t outCnt = 0;   // Float: components written
  uint8_t outBytes = 0; // bytes this attribute occupies in the decoded vertex
  ColorFmt colorFmt = ColorFmt::Rgba8;
  bool swap = true; // stored bytes are big-endian
  bool div3 = false;
  uint16_t outOff = 0;
  uint32_t srcOff = 0; // offset inside the raw vertex of the value (Direct) or of the index
  uint32_t within = 0; // byte offset inside the array element (indexed) - already added to srcOff for Direct
  uint32_t arrayStart = 0;
  uint32_t arrayStride = 0;
  float scale = 1.f;
  float constant = 0.f;
};

struct Ctx {
  const uint8_t* vtx;   // first raw vertex
  uint32_t vtxStride;   // bytes per raw vertex
  const uint8_t* arr;   // frame.storage
  size_t arrSize;
};

// ---- element decoders ---------------------------------------------------------------------------
// Each element knows how many source bytes it reads (kBytes), how to decode them and what to emit when the
// source is out of range (zero()).

template <typename T, bool SWAP>
inline float load_comp(const uint8_t* p) noexcept {
  if constexpr (std::is_same_v<T, float>) {
    uint32_t v = ld32(p);
    if constexpr (SWAP) {
      v = __builtin_bswap32(v);
    }
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
  } else if constexpr (sizeof(T) == 1) {
    return static_cast<float>(static_cast<T>(*p));
  } else {
    return static_cast<float>(static_cast<T>(rd16<SWAP>(p)));
  }
}

template <typename T, bool SWAP, int IN, int OUT>
struct FloatElem {
  static constexpr size_t kBytes = sizeof(T) * IN;
  float scale;
  void decode(const uint8_t* s, uint8_t* d) const noexcept {
    float f[OUT];
    for (int i = 0; i < IN; ++i) {
      float v = load_comp<T, SWAP>(s + i * sizeof(T));
      if constexpr (!std::is_same_v<T, float>) {
        v *= scale;
      }
      f[i] = v;
    }
    for (int i = IN; i < OUT; ++i) {
      f[i] = 0.f;
    }
    std::memcpy(d, f, sizeof(f));
  }
  void zero(uint8_t* d) const noexcept { std::memset(d, 0, OUT * sizeof(float)); }
};

template <bool DIV3>
struct MtxElem {
  static constexpr size_t kBytes = 1;
  void decode(const uint8_t* s, uint8_t* d) const noexcept {
    stf(d, static_cast<float>(DIV3 ? s[0] / 3u : s[0]));
  }
  void zero(uint8_t* d) const noexcept { st32(d, 0); }
};

template <ColorFmt FMT, bool SWAP>
struct ColorElem {
  static constexpr size_t kBytes = FMT == ColorFmt::Rgb565 || FMT == ColorFmt::Rgba4 ? 2
                                   : FMT == ColorFmt::Rgb8 || FMT == ColorFmt::Rgba6 ? 3
                                                                                     : 4;
  void decode(const uint8_t* s, uint8_t* d) const noexcept {
    if constexpr (FMT == ColorFmt::Rgba8) {
      st32(d, ld32(s));
    } else if constexpr (FMT == ColorFmt::Rgbx8) {
      st32(d, ld32(s) | 0xFF000000u);
    } else if constexpr (FMT == ColorFmt::Rgb8) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = 0xFF;
    } else if constexpr (FMT == ColorFmt::Rgb565) {
      const uint32_t v = rd16<SWAP>(s);
      d[0] = Tab5.v[(v >> 11) & 0x1F];
      d[1] = Tab6.v[(v >> 5) & 0x3F];
      d[2] = Tab5.v[v & 0x1F];
      d[3] = 0xFF;
    } else if constexpr (FMT == ColorFmt::Rgba4) {
      const uint32_t v = rd16<SWAP>(s);
      d[0] = Tab4.v[(v >> 12) & 0xF];
      d[1] = Tab4.v[(v >> 8) & 0xF];
      d[2] = Tab4.v[(v >> 4) & 0xF];
      d[3] = Tab4.v[v & 0xF];
    } else { // Rgba6: 24-bit value, big-endian unless the array says otherwise
      const uint32_t v = SWAP ? (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2]
                              : s[0] | (uint32_t(s[1]) << 8) | (uint32_t(s[2]) << 16);
      d[0] = Tab6.v[(v >> 18) & 0x3F];
      d[1] = Tab6.v[(v >> 12) & 0x3F];
      d[2] = Tab6.v[(v >> 6) & 0x3F];
      d[3] = Tab6.v[v & 0x3F];
    }
  }
  void zero(uint8_t* d) const noexcept { st32(d, 0); }
};

// ---- the attribute loop ---------------------------------------------------------------------------

template <int MODE, class Elem>
void run(const Plan& p, const Ctx& c, const Elem& e, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  const uint8_t* v = c.vtx + p.srcOff;
  for (uint32_t i = 0; i < n; ++i, v += c.vtxStride, out += outStride) {
    if constexpr (MODE == Direct) {
      e.decode(v, out);
    } else {
      const uint32_t idx = MODE == Idx8 ? v[0] : (uint32_t(v[0]) << 8) | v[1];
      const uint64_t a = uint64_t(p.arrayStart) + uint64_t(idx) * p.arrayStride + p.within;
      if (a + Elem::kBytes > c.arrSize) {
        e.zero(out);
      } else {
        e.decode(c.arr + a, out);
      }
    }
  }
}

template <int MODE, bool SWAP, typename T>
void run_float_cnt(const Plan& p, const Ctx& c, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  switch (p.inCnt * 4 + p.outCnt) {
  case 1 * 4 + 2:
    run<MODE>(p, c, FloatElem<T, SWAP, 1, 2>{p.scale}, out, outStride, n);
    break;
  case 2 * 4 + 2:
    run<MODE>(p, c, FloatElem<T, SWAP, 2, 2>{p.scale}, out, outStride, n);
    break;
  case 2 * 4 + 3:
    run<MODE>(p, c, FloatElem<T, SWAP, 2, 3>{p.scale}, out, outStride, n);
    break;
  case 3 * 4 + 3:
    run<MODE>(p, c, FloatElem<T, SWAP, 3, 3>{p.scale}, out, outStride, n);
    break;
  default:
    break;
  }
}

template <int MODE, bool SWAP>
void run_float(const Plan& p, const Ctx& c, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  switch (p.compType) {
  case GX_U8:
    run_float_cnt<MODE, false, uint8_t>(p, c, out, outStride, n);
    break;
  case GX_S8:
    run_float_cnt<MODE, false, int8_t>(p, c, out, outStride, n);
    break;
  case GX_U16:
    run_float_cnt<MODE, SWAP, uint16_t>(p, c, out, outStride, n);
    break;
  case GX_S16:
    run_float_cnt<MODE, SWAP, int16_t>(p, c, out, outStride, n);
    break;
  case GX_F32:
    run_float_cnt<MODE, SWAP, float>(p, c, out, outStride, n);
    break;
  default:
    break;
  }
}

template <int MODE, bool SWAP>
void run_color(const Plan& p, const Ctx& c, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  switch (p.colorFmt) {
  case ColorFmt::Rgb565:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgb565, SWAP>{}, out, outStride, n);
    break;
  case ColorFmt::Rgb8:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgb8, SWAP>{}, out, outStride, n);
    break;
  case ColorFmt::Rgbx8:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgbx8, SWAP>{}, out, outStride, n);
    break;
  case ColorFmt::Rgba4:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgba4, SWAP>{}, out, outStride, n);
    break;
  case ColorFmt::Rgba6:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgba6, SWAP>{}, out, outStride, n);
    break;
  case ColorFmt::Rgba8:
    run<MODE>(p, c, ColorElem<ColorFmt::Rgba8, SWAP>{}, out, outStride, n);
    break;
  }
}

template <int MODE>
void run_mtx(const Plan& p, const Ctx& c, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  if (p.div3) {
    run<MODE>(p, c, MtxElem<true>{}, out, outStride, n);
  } else {
    run<MODE>(p, c, MtxElem<false>{}, out, outStride, n);
  }
}

// Direct vertex data is always big-endian; indexed arrays carry their own endianness flag (Plan::swap).
void run_plan(const Plan& p, const Ctx& c, uint8_t* out, uint32_t outStride, uint32_t n) noexcept {
  out += p.outOff;
  switch (p.kind) {
  case PlanKind::Zero:
    for (uint32_t i = 0; i < n; ++i, out += outStride) {
      std::memset(out, 0, p.outBytes);
    }
    return;
  case PlanKind::Const: {
    uint32_t bits;
    std::memcpy(&bits, &p.constant, sizeof(bits));
    for (uint32_t i = 0; i < n; ++i, out += outStride) {
      st32(out, bits);
    }
    return;
  }
  case PlanKind::MtxIdx:
    switch (p.mode) {
    case Direct:
      run_mtx<Direct>(p, c, out, outStride, n);
      break;
    case Idx8:
      run_mtx<Idx8>(p, c, out, outStride, n);
      break;
    default:
      run_mtx<Idx16>(p, c, out, outStride, n);
      break;
    }
    return;
  case PlanKind::Float:
    switch (p.mode) {
    case Direct:
      run_float<Direct, true>(p, c, out, outStride, n);
      break;
    case Idx8:
      if (p.swap) {
        run_float<Idx8, true>(p, c, out, outStride, n);
      } else {
        run_float<Idx8, false>(p, c, out, outStride, n);
      }
      break;
    default:
      if (p.swap) {
        run_float<Idx16, true>(p, c, out, outStride, n);
      } else {
        run_float<Idx16, false>(p, c, out, outStride, n);
      }
      break;
    }
    return;
  case PlanKind::Color:
    switch (p.mode) {
    case Direct:
      run_color<Direct, true>(p, c, out, outStride, n);
      break;
    case Idx8:
      if (p.swap) {
        run_color<Idx8, true>(p, c, out, outStride, n);
      } else {
        run_color<Idx8, false>(p, c, out, outStride, n);
      }
      break;
    default:
      if (p.swap) {
        run_color<Idx16, true>(p, c, out, outStride, n);
      } else {
        run_color<Idx16, false>(p, c, out, outStride, n);
      }
      break;
    }
    return;
  }
}

// ---- plan construction ----------------------------------------------------------------------------

constexpr bool is_texmtx(GXAttr a) noexcept { return a >= GX_VA_TEX0MTXIDX && a <= GX_VA_TEX7MTXIDX; }
constexpr bool is_tex(GXAttr a) noexcept { return a >= GX_VA_TEX0 && a <= GX_VA_TEX7; }

uint32_t float_comp_size(uint8_t compType) noexcept {
  switch (compType) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  case GX_F32:
    return 4;
  default:
    return 0;
  }
}

bool color_fmt(uint8_t compType, ColorFmt& fmt, uint32_t& bytes) noexcept {
  switch (compType) {
  case GX_RGB565:
    fmt = ColorFmt::Rgb565;
    bytes = 2;
    return true;
  case GX_RGB8:
    fmt = ColorFmt::Rgb8;
    bytes = 3;
    return true;
  case GX_RGBX8:
    fmt = ColorFmt::Rgbx8;
    bytes = 4;
    return true;
  case GX_RGBA4:
    fmt = ColorFmt::Rgba4;
    bytes = 2;
    return true;
  case GX_RGBA6:
    fmt = ColorFmt::Rgba6;
    bytes = 3;
    return true;
  case GX_RGBA8:
    fmt = ColorFmt::Rgba8;
    bytes = 4;
    return true;
  default:
    return false;
  }
}

// Builds the decode plan for one layout entry. `slice` is 0/1/2 for the normal/binormal/tangent entries of an
// NBT normal. Anything that cannot be decoded safely becomes a Zero plan.
Plan make_plan(const PipelineConfig& config, const DrawData& draw, const DecodedAttr& e, uint32_t slice,
               uint32_t vtxStride) noexcept {
  Plan p;
  p.outOff = e.offset;
  p.outBytes = e.kind == DecodedAttr::Kind::Float ? static_cast<uint8_t>(e.components * 4) : 4;
  if (e.attr == GX_VA_NULL || static_cast<uint32_t>(e.attr) >= MaxVtxAttr) {
    return p; // line/point extras are filled by expand_lines()
  }
  const AttrConfig& m = config.shaderConfig.attrs[e.attr];
  if (m.attrType == GX_NONE) {
    if (e.attr == GX_VA_PNMTXIDX) {
      p.kind = PlanKind::Const;
      p.constant = static_cast<float>(draw.immediateData.currentPnMtx);
    }
    return p;
  }
  if (m.attrType != GX_DIRECT && m.attrType != GX_INDEX8 && m.attrType != GX_INDEX16) {
    return p;
  }

  const bool indexed = m.attrType != GX_DIRECT;
  const uint32_t idxBytes = m.attrType == GX_INDEX16 ? 2 : 1;
  uint32_t elemBytes = 0;
  uint32_t within = 0;   // offset inside the element
  uint32_t idxExtra = 0; // NBT3: separate index per slice
  uint32_t outCnt = 0;

  if (e.attr == GX_VA_PNMTXIDX || is_texmtx(e.attr)) {
    p.kind = PlanKind::MtxIdx;
    p.div3 = e.attr == GX_VA_PNMTXIDX;
    elemBytes = 1;
  } else if (e.attr == GX_VA_CLR0 || e.attr == GX_VA_CLR1) {
    if (!color_fmt(m.compType, p.colorFmt, elemBytes)) {
      return p;
    }
    p.kind = PlanKind::Color;
  } else {
    const uint32_t cs = float_comp_size(m.compType);
    if (cs == 0) {
      return p;
    }
    p.kind = PlanKind::Float;
    p.compType = m.compType;
    if (e.attr == GX_VA_POS) {
      if (m.cnt != 2 && m.cnt != 3) {
        p.kind = PlanKind::Zero;
        return p;
      }
      p.inCnt = m.cnt;
      outCnt = 3;
    } else if (e.attr == GX_VA_NRM) {
      if (m.cnt != 3 && m.cnt != 9) {
        p.kind = PlanKind::Zero;
        return p;
      }
      p.inCnt = 3;
      outCnt = 3;
      if (m.cnt == 9) {
        if (m.nbt3 && indexed) {
          // three indices (normal, binormal, tangent), each pointing at its own 3-component element
          idxExtra = slice * idxBytes;
        } else {
          within = slice * 3 * cs; // one element (or one inline run) holds all nine components
        }
      } else if (slice != 0) {
        p.kind = PlanKind::Zero;
        return p;
      }
    } else if (is_tex(e.attr)) {
      if (m.cnt != 1 && m.cnt != 2) {
        p.kind = PlanKind::Zero;
        return p;
      }
      p.inCnt = m.cnt;
      outCnt = 2;
    } else {
      p.kind = PlanKind::Zero;
      return p;
    }
    p.outCnt = static_cast<uint8_t>(outCnt);
    elemBytes = cs * p.inCnt;
    p.scale = std::ldexp(1.f, -static_cast<int>(m.frac & 31));
  }

  if (!indexed) {
    p.mode = Direct;
    p.swap = true; // direct vertex data is big-endian
    p.srcOff = m.offset + within;
    if (p.srcOff + elemBytes > vtxStride) {
      p.kind = PlanKind::Zero;
    }
    return p;
  }

  // Indexed: the vertex holds the index; the element lives in the snapshotted array.
  p.mode = m.attrType == GX_INDEX8 ? Idx8 : Idx16;
  p.swap = !m.le;
  p.srcOff = m.offset + idxExtra;
  p.within = within;
  p.arrayStride = m.stride;
  if (e.attr < GX_VA_POS || e.attr > GX_VA_TEX7 || p.srcOff + idxBytes > vtxStride) {
    p.kind = PlanKind::Zero; // matrix indices cannot be indexed, nor can a truncated vertex be trusted
    return p;
  }
  p.arrayStart = draw.immediateData.arrayStart[e.attr - GX_VA_POS];
  return p;
}

} // namespace

// ---- layout ---------------------------------------------------------------------------------------

DecodedLayout decoded_layout(const ShaderConfig& config) noexcept {
  static constexpr const char* TexMtxNames[8] = {"aTexMtxIdx0", "aTexMtxIdx1", "aTexMtxIdx2", "aTexMtxIdx3",
                                                 "aTexMtxIdx4", "aTexMtxIdx5", "aTexMtxIdx6", "aTexMtxIdx7"};
  static constexpr const char* TexNames[8] = {"aTex0", "aTex1", "aTex2", "aTex3",
                                              "aTex4", "aTex5", "aTex6", "aTex7"};
  DecodedLayout layout;
  uint32_t offset = 0;
  const auto add = [&](GXAttr attr, DecodedAttr::Kind kind, uint8_t comps, const char* name) {
    if (layout.count >= MaxDecodedAttrs) {
      return;
    }
    layout.attrs[layout.count++] = DecodedAttr{attr, kind, comps, static_cast<uint16_t>(offset), name};
    offset += kind == DecodedAttr::Kind::Float ? comps * 4u : 4u;
    layout.stride = static_cast<uint16_t>(offset);
  };
  using Kind = DecodedAttr::Kind;
  const auto present = [&](GXAttr a) { return config.attrs[a].attrType != GX_NONE; };

  add(GX_VA_PNMTXIDX, Kind::Float, 1, "aPnMtxIdx"); // always present, see vita_gx.hpp
  for (int i = 0; i < 8; ++i) {
    const auto a = static_cast<GXAttr>(GX_VA_TEX0MTXIDX + i);
    if (present(a)) {
      add(a, Kind::Float, 1, TexMtxNames[i]);
    }
  }
  if (present(GX_VA_POS)) {
    add(GX_VA_POS, Kind::Float, 3, "aPos");
  }
  if (present(GX_VA_NRM)) {
    add(GX_VA_NRM, Kind::Float, 3, "aNrm");
    if (config.attrs[GX_VA_NRM].cnt == 9) {
      add(GX_VA_NRM, Kind::Float, 3, "aBin");
      add(GX_VA_NRM, Kind::Float, 3, "aTan");
    }
  }
  if (present(GX_VA_CLR0)) {
    add(GX_VA_CLR0, Kind::UByte4Norm, 4, "aClr0");
  }
  if (present(GX_VA_CLR1)) {
    add(GX_VA_CLR1, Kind::UByte4Norm, 4, "aClr1");
  }
  for (int i = 0; i < 8; ++i) {
    const auto a = static_cast<GXAttr>(GX_VA_TEX0 + i);
    if (present(a)) {
      add(a, Kind::Float, 2, TexNames[i]);
    }
  }
  if (config.lineMode != 0) {
    add(GX_VA_NULL, Kind::Float, 1, "aCorner");
    if (config.lineMode != 3) {
      add(GX_VA_NULL, Kind::Float, 3, "aPosOther");
      add(GX_VA_NULL, Kind::Float, 1, "aPnMtxIdxOther");
    }
  }
  return layout;
}

// ---- decode -----------------------------------------------------------------------------------------

// ---- content-addressed vertex cache -------------------------------------------------------------
//
// The decode is expensive: every GX vertex is expanded from big-endian, indexed, fixed-point source
// into interleaved floats. The game re-sends much of the same geometry every frame (static display
// lists, resident models), so the same source bytes are decoded over and over.
//
// The cache CANNOT be keyed on the recorded offsets. `push_storage` rebuilds `Frame::storage` every
// frame, so an offset that meant byte X in frame N means nothing in frame N+1. It is keyed on the
// source CONTENT instead: the exact bytes the decode would read, copied transparently. A hit is only
// accepted when those bytes are byte-identical to what was decoded last time, which is precisely the
// condition under which the decode result is identical too. That makes the cache sound without any
// knowledge of which display lists or arrays the game considers static.
//
// WHAT THE KEY MUST COVER, and the two ways the first version got it wrong (both found by reading the
// decoder back rather than by a test, which is exactly why they are written down here):
//
//  1. Indexed attributes read from `frame.storage`, not `frame.verts` (Ctx::arr). The first version
//     sized the "source span" as vertices + indexed_array_bytes() but then hashed and copied that
//     many bytes starting at `frame.verts[vertStart]`. That was an overread past the vertex range,
//     and the array bytes that the decode actually reads were never part of the key at all. Two
//     draws with identical index bytes but different snapshotted arrays could therefore FALSE-HIT
//     and get each other's positions. array_ranges() below fixes this: each addressable array span is
//     hashed into a digest that goes into the key, and its bytes are copied into the entry.
//  2. The decode also depends on the pipeline and on `draw.immediateData` (currentPnMtx and the
//     per-attribute arrayStart), not just on source bytes. Two pipelines over identical vertex bytes
//     decode differently. Both are hashed in.
//
// A consequence worth stating plainly: every measurement taken with the first version of this key is
// void, including the 83.8% hit rate it reported. A false hit is the worst possible failure for a
// cache, because it is silent and it renders the wrong geometry. Any future hit-rate number has to be
// re-taken with the key below.
//
// Validity is hashing first and memcmp only on a hash match: a miss costs a string hash of the source
// span rather than a memcmp, and a hit costs a hash plus a few memcmps instead of a full decode.
// Whether that is actually a win is measured, not assumed -- see the accumulators in VertexCacheStats
// and vertex_cache_report().

namespace {

// One entry per draw in flight is not enough (a frame has ~170 draws and the scene repeats between
// frames), so this is a 4-way set-associative table indexed by content hash: 512 entries, 4 ways.
// DuskOnVita arrived at the same shape independently. A direct-mapped table was tried first and hit
// ~50% while logging one eviction per miss, i.e. the slots were thrashing each other rather than being
// full; ways let the handful of hot draws in a scene coexist instead of colliding. Collisions and
// aliasing are harmless, because the memcmp fails and the draw simply decodes normally.
// DEFAULT OFF, and not because it does not work. Measured in Vita3K at the same scene (frames 1080-1200,
// ~165 draws/frame, 71 programs), comparing the decode cost the cache reports against itself:
//
//   cache OFF   decode 474.3 ms / 120 frames = 3.95 ms/frame
//   cache ON    decode 382.9 + overhead 177.1 = 560.0 ms / 120 = 4.67 ms/frame
//   cache OFF   decode 773.4 ms / 120 frames = 6.45 ms/frame   (a heavier window)
//   cache ON    decode 344.3 + overhead 235.8 = 580.1 ms / 120 = 4.83 ms/frame
//
// Halving the decode is real (74% hit rate), but the whole vertex path is only 4-6 ms out of a ~130 ms
// frame here, so the absolute change is within +-1.5 ms and the frame time moves less than the run-to-run
// noise. On this evidence it is not a win and it is not enabled.
//
// The honest caveat is the one that matters: Vita3K executes guest ARM on a fast host CPU, so the decode
// here is much cheaper than on a Cortex-A9. DuskOnVita profiled 66 ms of vertex work in a 154 ms frame on
// a real Vita 1000 -- about an order of magnitude more than this emulator shows, which is consistent with
// the host being that much faster. If the hardware figure is right, this cache is worth 15-45 ms/frame
// there and worth turning on. That is a decision for a run on a console, not for this emulator.
//
// Set to 0 to disable the cache entirely (A/B measurement). With 0 every draw decodes as before.
constexpr int kVertexCacheEnabled = 0;
constexpr uint32_t kVertexCacheWays = kVertexCacheEnabled ? 4u : 1u;
constexpr uint32_t kVertexCacheSets = 512 / kVertexCacheWays;
// Source bytes copied per entry. Sized from measurement: the largest single draw's source span seen in
// the opening area is far below this, and the cap keeps the whole cache inside ~10 MiB.
constexpr uint32_t kVertexCacheSourceMax = 32768;

struct VertexCacheEntry {
  uint64_t key = 0;          // hash of vertices AND arrays AND pipeline AND immediate data
  uint64_t storageDigest = 0; // digest of the assisted array contents, for the memcmp below
  uint32_t sourceBytes = 0;   // vertex bytes copied into `source`
  uint32_t storageBytes = 0;  // array bytes copied into `storage`
  uint32_t stride = 0;
  uint32_t vtxCount = 0;
  uint32_t outBytes = 0;
  uint8_t* source = nullptr;  // the exact vertex source span, copied so it survives the frame
  uint8_t* storage = nullptr; // the exact array bytes this draw addresses, likewise
  uint8_t* decoded = nullptr; // the decode result for that input
  uint64_t use = 0;
};

// A draw can index into several vertex arrays at independent offsets. Rather than copying all of
// frame.storage (which the whole frame shares), copy only the bytes this draw can address, per array.
struct ArrayRange {
  const uint8_t* ptr = nullptr;
  uint32_t bytes = 0;
  uint32_t offset = 0; // where it lands inside the entry's array blob
};
constexpr uint32_t kMaxArrayRanges = MaxIndexAttr;

VertexCacheEntry gVertexCache[kVertexCacheSets * kVertexCacheWays];

// Returns the way in this set that matches, or nullptr. Sets are indexed by key; the memcmp is the
// authority for the vertex bytes and `digest` covers the array bytes (which the caller has already
// folded into `key` as well, from the same digest, so a stale or colliding digest cannot both agree).
VertexCacheEntry* vertex_cache_find(uint64_t key, uint64_t digest, const uint8_t* src, size_t srcBytes,
                                    size_t storageBytes, uint32_t stride, uint32_t vtxCount,
                                    size_t outBytes) noexcept {
  VertexCacheEntry* set = &gVertexCache[(key % kVertexCacheSets) * kVertexCacheWays];
  for (uint32_t w = 0; w < kVertexCacheWays; ++w) {
    VertexCacheEntry& e = set[w];
    if (e.key != key || e.stride != stride || e.vtxCount != vtxCount || e.sourceBytes != srcBytes ||
        e.storageBytes != storageBytes || e.outBytes != outBytes || e.storageDigest != digest) {
      continue;
    }
    if (std::memcmp(e.source, src, srcBytes) == 0) {
      return &e;
    }
  }
  return nullptr;
}

// Picks the victim: an unused way, else the least recently used.
VertexCacheEntry* vertex_cache_victim(uint64_t key) noexcept {
  VertexCacheEntry* set = &gVertexCache[(key % kVertexCacheSets) * kVertexCacheWays];
  VertexCacheEntry* oldest = &set[0];
  for (uint32_t w = 0; w < kVertexCacheWays; ++w) {
    if (set[w].key == 0) {
      return &set[w];
    }
    if (set[w].use < oldest->use) {
      oldest = &set[w];
    }
  }
  return oldest;
}
uint64_t gVertexCacheClock = 0;
uint32_t gVertexCacheBytes = 0;
constexpr uint32_t kVertexCacheBudget = 12u * 1024u * 1024u;

struct VertexCacheStats {
  uint64_t lookups = 0, hits = 0, misses = 0, evictions = 0, rejectedTooBig = 0, budgetDeferrals = 0;
};
VertexCacheStats gVertexCacheStats;
// Extra source bytes an indexed pipeline reads from the snapshotted arrays. Only the attributes this
// layout actually indexes contribute, and only over the range the draw's indices can address.
uint32_t indexed_array_bytes(const PipelineConfig& config, const DrawData& draw, uint32_t vtxStride) noexcept {
  uint32_t total = 0;
  const auto& sc = config.shaderConfig;
  for (uint32_t a = 0; a < MaxVtxAttr; ++a) {
    const AttrConfig& m = sc.attrs[a];
    if (m.attrType != GX_INDEX8 && m.attrType != GX_INDEX16) {
      continue;
    }
    if (a < GX_VA_POS || a > GX_VA_TEX7) {
      continue;
    }
    const uint32_t idxBytes = m.attrType == GX_INDEX16 ? 2 : 1;
    if (vtxStride == 0 || m.offset + idxBytes > vtxStride) {
      continue;
    }
    // Conservative: one element per vertex, plus the element size, clipped to the recorded storage.
    const uint32_t elems = draw.vtxCount + 1;
    const uint64_t span = static_cast<uint64_t>(elems) * (m.stride != 0 ? m.stride : 1);
    total += static_cast<uint32_t>(std::min<uint64_t>(span, 1u << 20));
  }
  return total;
}

inline uint64_t hash_bytes(const uint8_t* p, size_t n) noexcept {
  // Word-at-a-time, not FNV-1a. The first version hashed byte by byte with a serial multiply chain,
  // and over 32 KB of source per draw that cost more than the decode it was meant to avoid: measured
  // cache-on runs were SLOWER than cache-off at identical draw counts. Two independent accumulators
  // over 32-bit words break the dependency chain and shrink the loop by 4x; the hash only needs to be
  // a filter, because the memcmp is the authority.
  uint64_t a = 0xcbf29ce484222325ull;
  uint64_t b = 0x9e3779b97f4a7c15ull;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, sizeof(w));
    a = (a ^ w) * 0x100000001b3ull;
    b = (b + w) * 0xff51afd7ed558ccdull;
  }
  for (; i < n; ++i) {
    a = (a ^ p[i]) * 0x100000001b3ull;
  }
  b ^= a >> 29;
  b *= 0xc4ceb9fe1a85ec53ull;
  b ^= a;
  return b;
}

// Key of the draw currently being decoded; 0 means "do not cache this one".
uint64_t gVertexCachePendingKey = 0;
uint64_t gVertexCachePendingDigest = 0;

void free_vertex_cache_entry(VertexCacheEntry& e) noexcept {
  gVertexCacheBytes -= e.sourceBytes + e.storageBytes + e.outBytes;
  delete[] e.source;
  delete[] e.storage;
  delete[] e.decoded;
  e = VertexCacheEntry{};
}

// Builds the list of addressable array ranges for a draw. Replaces the earlier indexed_array_bytes(),
// which returned only a SIZE and was then used to read bytes out of the wrong buffer.
uint32_t array_ranges(const PipelineConfig& config, const DrawData& draw, const gfx::vita::Frame& frame,
                      ArrayRange (&out)[kMaxArrayRanges]) noexcept {
  uint32_t count = 0;
  uint32_t offset = 0;
  const auto& sc = config.shaderConfig;
  for (uint32_t a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
    const AttrConfig& m = sc.attrs[a];
    if (m.attrType != GX_INDEX8 && m.attrType != GX_INDEX16) {
      continue;
    }
    // The largest element any index of this draw can reach, matching what run<>.decode() will read.
    const uint64_t elems = static_cast<uint64_t>(draw.vtxCount) + 1;
    const uint64_t elemBytes = (m.stride != 0 ? m.stride : 1) + 16;
    const uint64_t span = draw.immediateData.arrayStart[a - GX_VA_POS] + elems * elemBytes;
    if (span == 0 || span > frame.storage.size()) {
      // Out of range: the decoder zero-fills, so no bytes are read and nothing needs keying.
      continue;
    }
    out[count++] = ArrayRange{frame.storage.data() + draw.immediateData.arrayStart[a - GX_VA_POS],
                              static_cast<uint32_t>(span - draw.immediateData.arrayStart[a - GX_VA_POS]), offset};
    offset += out[count - 1].bytes;
    if (count == kMaxArrayRanges) {
      break;
    }
  }
  return count;
}

// Folds the pipeline and the draw's immediate data into the key. Two draws can share vertex bytes and
// still decode differently, so this is not optional.
uint64_t config_key(const PipelineConfig& config, const DrawData& draw) noexcept {
  uint64_t k = hash_bytes(reinterpret_cast<const uint8_t*>(&config), sizeof(config));
  k ^= hash_bytes(reinterpret_cast<const uint8_t*>(&draw.immediateData), sizeof(draw.immediateData));
  k ^= static_cast<uint64_t>(draw.vtxCount) * 0x9e3779b97f4a7c15ull;
  return k == 0 ? 1 : k;
}

// Self-measurement. Whether the cache earns its keep is a question about milliseconds, and whole-frame
// wall time in the emulator is far too noisy to answer it (the same build measured 171-504 ms/frame).
// These accumulators make the trade explicit: decoding avoided, versus hashing and copying paid.
uint64_t gCacheNanosHash = 0;
uint64_t gCacheNanosDecode = 0;
uint64_t gCacheNanosCopy = 0;
uint64_t gCacheDecodedVerts = 0;

} // namespace

void vertex_cache_clear() noexcept {
  for (auto& e : gVertexCache) {
    free_vertex_cache_entry(e);
  }
  gVertexCacheBytes = 0;
}

void vertex_cache_report(const char* label) noexcept {
  const auto& s = gVertexCacheStats;
  const double hitPct = s.lookups != 0 ? 100.0 * static_cast<double>(s.hits) / static_cast<double>(s.lookups) : 0.0;
  const double hashMs = static_cast<double>(gCacheNanosHash) / 1.0e6;
  const double decodeMs = static_cast<double>(gCacheNanosDecode) / 1.0e6;
  const double copyMs = static_cast<double>(gCacheNanosCopy) / 1.0e6;
  // spent = hash + copy on hits, hash + copy(store) on misses; saved = the decode a hit avoided.
  // Net is spent - saved, so a negative net means the cache is paying for itself.
  Log.warn("vertex cache {}: {} lookups, {} hits ({:.1f}%), {} misses, {} evictions, {} too-big, "
           "{} budget-deferred, {} KiB held",
           label, s.lookups, s.hits, hitPct, s.misses, s.evictions, s.rejectedTooBig, s.budgetDeferrals,
           gVertexCacheBytes >> 10);
  // The ceiling, which is the number that actually decides this: `decodeMs` is the total decode cost of
  // the windows, i.e. the most the cache could ever save if every miss became a free hit. Reporting that
  // against the measured hash+copy cost bounds the win without needing a low-noise frame timer, which
  // this emulator cannot provide.
  const double overheadMs = hashMs + copyMs;
  Log.warn("vertex cache {} cost: overhead {:.1f} ms (hash {:.1f} + copy {:.1f}), decode {:.1f} ms over {} verts",
           label, overheadMs, hashMs, copyMs, decodeMs, gCacheDecodedVerts);
  Log.warn("vertex cache {} ceiling: eliminating 100% of the remaining decode would save at most {:.1f} ms, "
           "i.e. net no better than {:.1f} ms",
           label, decodeMs, decodeMs - overheadMs);
  gCacheNanosHash = gCacheNanosDecode = gCacheNanosCopy = 0;
  gCacheDecodedVerts = 0;
}

void decode_draw(const PipelineConfig& config, const DrawData& draw, const gfx::vita::Frame& frame,
                 const DecodedLayout& layout, std::vector<uint8_t>& out) {
  const uint32_t stride = layout.stride;
  const uint32_t vtxCount = draw.vtxCount;
  out.resize(static_cast<size_t>(vtxCount) * stride);
  if (vtxCount == 0 || stride == 0) {
    return;
  }

  // How many raw vertices actually exist in the recorded buffer (a truncated range decodes as zeros).
  const uint32_t vtxStride = config.shaderConfig.vtxStride;
  const size_t vertBytes = frame.verts.size();
  const size_t vertStart = std::min<size_t>(draw.vertRange.offset, vertBytes);
  const size_t availBytes = vertBytes - vertStart;
  uint32_t n = vtxCount;
  if (vtxStride != 0) {
    n = static_cast<uint32_t>(std::min<size_t>(vtxCount, availBytes / vtxStride));
  } else if (availBytes == 0) {
    n = 0;
  }
  uint8_t* dst = out.data();
  if (n < vtxCount) {
    std::memset(dst + static_cast<size_t>(n) * stride, 0, static_cast<size_t>(vtxCount - n) * stride);
  }
  if (n == 0) {
    return;
  }

  // The two separate source spans this draw reads, and they are NOT in the same buffer:
  //   vertices -> frame.verts[vertStart ...]          (the recorded vertex range)
  //   arrays   -> frame.storage[arrayStart[attr] ...] (only for indexed attributes)
  const size_t srcBytes = vtxStride != 0 ? static_cast<size_t>(n) * vtxStride : 0;
  const uint8_t* src = frame.verts.data() + vertStart;
  ArrayRange ranges[kMaxArrayRanges];
  const uint32_t rangeCount = array_ranges(config, draw, frame, ranges);
  size_t storageBytes = 0;
  for (uint32_t i = 0; i < rangeCount; ++i) {
    storageBytes += ranges[i].bytes;
  }

  uint64_t key = 0;
  const bool cacheable = kVertexCacheEnabled != 0 && srcBytes != 0 && srcBytes <= kVertexCacheSourceMax &&
                         storageBytes <= kVertexCacheSourceMax;
  if (cacheable) {
    const auto hashStart = std::chrono::steady_clock::now();
    key = config_key(config, draw);
    key ^= hash_bytes(src, srcBytes);
    uint64_t digest = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < rangeCount; ++i) {
      digest = (digest ^ hash_bytes(ranges[i].ptr, ranges[i].bytes)) * 0x100000001b3ull;
      key ^= digest;
    }
    if (key == 0) {
      key = 1; // 0 is the "no entry" sentinel
    }
    gCacheNanosHash += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hashStart).count());
    ++gVertexCacheStats.lookups;
    if (VertexCacheEntry* hit =
            vertex_cache_find(key, digest, src, srcBytes, storageBytes, stride, vtxCount, out.size())) {
      // Byte-identical vertices, pipeline and immediate data, and identical array contents: the
      // decode result is identical by construction. The memcmp inside find() proves the first two;
      // `key` covers the arrays, which the digest folded in.
      ++gVertexCacheStats.hits;
      hit->use = ++gVertexCacheClock;
      const auto copyStart = std::chrono::steady_clock::now();
      std::memcpy(out.data(), hit->decoded, out.size());
      gCacheNanosCopy += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copyStart).count());
      return;
    }
    ++gVertexCacheStats.misses;
    // Store AFTER decoding, below, so a failed decode can never populate the cache.
    gVertexCachePendingKey = key;
    gVertexCachePendingDigest = digest;
  } else {
    gVertexCachePendingKey = 0;
    if (srcBytes > kVertexCacheSourceMax || storageBytes > kVertexCacheSourceMax) {
      ++gVertexCacheStats.rejectedTooBig;
    }
  }

  {
    const auto decodeStart = std::chrono::steady_clock::now();
    const Ctx ctx{frame.verts.data() + vertStart, vtxStride, frame.storage.data(), frame.storage.size()};
    uint32_t nrmSlice = 0;
    for (uint32_t i = 0; i < layout.count; ++i) {
      const DecodedAttr& e = layout.attrs[i];
      const uint32_t slice = e.attr == GX_VA_NRM ? nrmSlice++ : 0;
      const Plan p = make_plan(config, draw, e, slice, vtxStride);
      run_plan(p, ctx, dst, stride, n);
    }
    gCacheNanosDecode += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeStart).count());
    gCacheDecodedVerts += n;
  }

  if (gVertexCachePendingKey != 0) {
    VertexCacheEntry& slot = *vertex_cache_victim(gVertexCachePendingKey);
    const uint32_t needed =
        static_cast<uint32_t>(srcBytes + storageBytes + static_cast<size_t>(out.size()));
    if (gVertexCacheBytes + needed > kVertexCacheBudget) {
      // Refuse rather than evict cyclically: a scene larger than the budget would otherwise replace its
      // own start with its own end every frame and never hit. Excess draws simply decode normally.
      ++gVertexCacheStats.budgetDeferrals;
    } else {
      if (slot.key != 0) {
        ++gVertexCacheStats.evictions;
      }
      free_vertex_cache_entry(slot);
      const auto storeStart = std::chrono::steady_clock::now();
      slot.source = new (std::nothrow) uint8_t[srcBytes];
      slot.storage = new (std::nothrow) uint8_t[storageBytes];
      slot.decoded = new (std::nothrow) uint8_t[out.size()];
      if (slot.source == nullptr || slot.decoded == nullptr || (storageBytes != 0 && slot.storage == nullptr)) {
        free_vertex_cache_entry(slot);
      } else {
        std::memcpy(slot.source, src, srcBytes);
        size_t at = 0;
        for (uint32_t i = 0; i < rangeCount; ++i) {
          std::memcpy(slot.storage + at, ranges[i].ptr, ranges[i].bytes);
          at += ranges[i].bytes;
        }
        std::memcpy(slot.decoded, out.data(), out.size());
        slot.key = gVertexCachePendingKey;
        slot.storageDigest = gVertexCachePendingDigest;
        slot.sourceBytes = static_cast<uint32_t>(srcBytes);
        slot.storageBytes = static_cast<uint32_t>(storageBytes);
        slot.stride = stride;
        slot.vtxCount = vtxCount;
        slot.outBytes = static_cast<uint32_t>(out.size());
        slot.use = ++gVertexCacheClock;
        gVertexCacheBytes += needed;
      }
      gCacheNanosCopy += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - storeStart).count());
    }
    gVertexCachePendingKey = 0;
  }
}

// ---- lines and points -----------------------------------------------------------------------------

void expand_lines(const PipelineConfig& config, const DrawData& draw, const gfx::vita::Frame& frame,
                  const DecodedLayout& layout, std::vector<ExpandedPrimitives>& chunks) {
  chunks.clear();
  const uint32_t lineMode = config.shaderConfig.lineMode;
  if (lineMode == 0 || layout.stride == 0) {
    return;
  }
  const uint32_t vtxCount = draw.vtxCount;
  uint32_t segments = 0;
  if (lineMode == 1) {
    segments = vtxCount / 2;
  } else if (lineMode == 2) {
    segments = vtxCount >= 2 ? vtxCount - 1 : 0;
  } else {
    segments = vtxCount;
  }
  if (segments == 0) {
    return;
  }

  // Decode every raw vertex once, then replicate endpoints into quads.
  std::vector<uint8_t> raw;
  decode_draw(config, draw, frame, layout, raw);

  const uint32_t stride = layout.stride;
  int posOff = -1, pnmOff = -1, cornerOff = -1, posOtherOff = -1, pnmOtherOff = -1;
  for (uint32_t i = 0; i < layout.count; ++i) {
    const DecodedAttr& e = layout.attrs[i];
    if (e.attr == GX_VA_POS) {
      posOff = e.offset;
    } else if (e.attr == GX_VA_PNMTXIDX) {
      pnmOff = e.offset;
    } else if (e.attr == GX_VA_NULL) {
      if (std::strcmp(e.name, "aCorner") == 0) {
        cornerOff = e.offset;
      } else if (std::strcmp(e.name, "aPosOther") == 0) {
        posOtherOff = e.offset;
      } else if (std::strcmp(e.name, "aPnMtxIdxOther") == 0) {
        pnmOtherOff = e.offset;
      }
    }
  }

  constexpr uint32_t QuadsPerChunk = MaxChunkVertices / 4;
  for (uint32_t first = 0; first < segments; first += QuadsPerChunk) {
    const uint32_t quads = std::min(QuadsPerChunk, segments - first);
    ExpandedPrimitives& chunk = chunks.emplace_back();
    chunk.vertices.resize(static_cast<size_t>(quads) * 4 * stride);
    chunk.indices.resize(static_cast<size_t>(quads) * 6);
    uint8_t* vtx = chunk.vertices.data();
    uint16_t* idx = chunk.indices.data();
    for (uint32_t q = 0; q < quads; ++q) {
      const uint32_t seg = first + q;
      const uint32_t a = lineMode == 1 ? seg * 2 : seg;
      const uint32_t b = lineMode == 3 ? a : a + 1;
      const uint32_t base = q * 4;
      for (uint32_t corner = 0; corner < 4; ++corner) {
        const bool useB = lineMode != 3 && corner >= 2;
        const uint32_t self = useB ? b : a;
        const uint32_t other = useB ? a : b;
        const uint8_t* src = raw.data() + static_cast<size_t>(self) * stride;
        uint8_t* d = vtx + static_cast<size_t>(base + corner) * stride;
        std::memcpy(d, src, stride);
        if (cornerOff >= 0) {
          stf(d + cornerOff, static_cast<float>(corner));
        }
        if (lineMode != 3) {
          const uint8_t* oth = raw.data() + static_cast<size_t>(other) * stride;
          if (posOtherOff >= 0) {
            if (posOff >= 0) {
              std::memcpy(d + posOtherOff, oth + posOff, 3 * sizeof(float));
            } else {
              std::memset(d + posOtherOff, 0, 3 * sizeof(float));
            }
          }
          if (pnmOtherOff >= 0) {
            if (pnmOff >= 0) {
              std::memcpy(d + pnmOtherOff, oth + pnmOff, sizeof(float));
            } else {
              std::memset(d + pnmOtherOff, 0, sizeof(float));
            }
          }
        }
      }
      idx[0] = static_cast<uint16_t>(base + 0);
      idx[1] = static_cast<uint16_t>(base + 1);
      idx[2] = static_cast<uint16_t>(base + 2);
      idx[3] = static_cast<uint16_t>(base + 2);
      idx[4] = static_cast<uint16_t>(base + 1);
      idx[5] = static_cast<uint16_t>(base + 3);
      idx += 6;
    }
  }
}

} // namespace aurora::gx::vita
