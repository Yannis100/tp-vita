// Host tests for gx/vita_vertex.cpp (decoded_layout / decode_draw / expand_lines).
//
// The reference decoder below is deliberately written independently of the implementation: byte-by-byte
// assembly of every value, double arithmetic, no templates, a layout table recomputed from the contract text in
// gx/vita_gx.hpp. Random vertex formats cover every attribute type / component type / count / color format /
// direct and indexed addressing / array endianness / NBT variants.
//
//   tests/host/vertex/run.sh            run tests (+ AddressSanitizer / UBSan)
//   tests/host/vertex/run.sh bench      also print decode throughput
#include <gx/vita_gx.hpp>

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace aurora;
using namespace aurora::gx;
using namespace aurora::gx::vita;
using gfx::vita::Frame;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK_T(cond, ...)                                                                                           \
  do {                                                                                                               \
    ++gChecks;                                                                                                       \
    if (!(cond)) {                                                                                                   \
      ++gFailures;                                                                                                   \
      if (gFailures < 40) {                                                                                          \
        std::printf("FAIL %s:%d: %s\n  ", __FILE__, __LINE__, #cond);                                                \
        std::printf(__VA_ARGS__);                                                                                    \
        std::printf("\n");                                                                                           \
      }                                                                                                              \
    }                                                                                                                \
  } while (0)

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

// ---- independent reference ---------------------------------------------------------------------------

static uint32_t ref_comp_size(uint8_t comp) {
  switch (comp) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  default:
    return 4;
  }
}
static uint32_t ref_color_size(uint8_t fmt) {
  switch (fmt) {
  case GX_RGB565:
  case GX_RGBA4:
    return 2;
  case GX_RGB8:
  case GX_RGBA6:
    return 3;
  default:
    return 4;
  }
}

static uint32_t ref_uint(const uint8_t* p, int bytes, bool littleEndian) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) {
    const uint32_t b = p[littleEndian ? bytes - 1 - i : i];
    v = (v << 8) | b;
  }
  return v;
}

static uint32_t ref_round_scale(uint32_t v, uint32_t max) {
  return static_cast<uint32_t>(std::floor(static_cast<double>(v) * 255.0 / max + 0.5));
}

static void ref_color(const uint8_t* p, uint8_t fmt, bool le, uint8_t out[4]) {
  switch (fmt) {
  case GX_RGB565: {
    const uint32_t v = ref_uint(p, 2, le);
    out[0] = ref_round_scale((v >> 11) & 31, 31);
    out[1] = ref_round_scale((v >> 5) & 63, 63);
    out[2] = ref_round_scale(v & 31, 31);
    out[3] = 255;
    break;
  }
  case GX_RGB8:
    out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255;
    break;
  case GX_RGBX8:
    out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255;
    break;
  case GX_RGBA4: {
    const uint32_t v = ref_uint(p, 2, le);
    out[0] = ref_round_scale((v >> 12) & 15, 15);
    out[1] = ref_round_scale((v >> 8) & 15, 15);
    out[2] = ref_round_scale((v >> 4) & 15, 15);
    out[3] = ref_round_scale(v & 15, 15);
    break;
  }
  case GX_RGBA6: {
    const uint32_t v = ref_uint(p, 3, le);
    out[0] = ref_round_scale((v >> 18) & 63, 63);
    out[1] = ref_round_scale((v >> 12) & 63, 63);
    out[2] = ref_round_scale((v >> 6) & 63, 63);
    out[3] = ref_round_scale(v & 63, 63);
    break;
  }
  default: // GX_RGBA8
    std::memcpy(out, p, 4);
    break;
  }
}

static float ref_float(const uint8_t* p, uint8_t comp, bool le, uint8_t frac) {
  uint32_t u;
  switch (comp) {
  case GX_U8:
    return static_cast<float>(std::ldexp(static_cast<double>(p[0]), -static_cast<int>(frac & 31)));
  case GX_S8:
    return static_cast<float>(std::ldexp(static_cast<double>(static_cast<int8_t>(p[0])), -static_cast<int>(frac & 31)));
  case GX_U16:
    u = ref_uint(p, 2, le);
    return static_cast<float>(std::ldexp(static_cast<double>(u), -static_cast<int>(frac & 31)));
  case GX_S16:
    u = ref_uint(p, 2, le);
    return static_cast<float>(std::ldexp(static_cast<double>(static_cast<int16_t>(u)), -static_cast<int>(frac & 31)));
  default: {
    u = ref_uint(p, 4, le);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  }
}

// Format description used by the generator and the reference.
struct Spec {
  uint8_t type = GX_NONE; // GXAttrType
  uint8_t comp = 0;       // GXCompType (or color format for CLRn)
  uint8_t cnt = 0;        // as AttrConfig::cnt
  uint8_t frac = 0;
  uint8_t arrayStride = 0;
  bool le = false; // array endianness
  bool nbt3 = false;
};

struct Case {
  Spec spec[GX_VA_TEX7 + 1];
  PipelineConfig config;
  DrawData draw{};
  Frame frame;
  uint32_t vtxCount = 0;
  uint32_t vtxStride = 0;
  uint32_t arrayElems[GX_VA_TEX7 + 1] = {};
  uint32_t arrayBase[GX_VA_TEX7 + 1] = {};
};

static bool is_clr(int a) { return a == GX_VA_CLR0 || a == GX_VA_CLR1; }
static bool is_mtx(int a) { return a == GX_VA_PNMTXIDX || (a >= GX_VA_TEX0MTXIDX && a <= GX_VA_TEX7MTXIDX); }

// Bytes of one value (direct) or element (indexed) of the attribute.
static uint32_t ref_elem_bytes(int a, const Spec& s) {
  if (is_mtx(a)) return 1;
  if (is_clr(a)) return ref_color_size(s.comp);
  return ref_comp_size(s.comp) * s.cnt;
}

static void gen_spec(Rng& rng, int a, Spec& s, bool linesOnlyBasic) {
  s = {};
  auto pickType = [&](bool allowNone, bool allowIdx) {
    if (allowNone && rng.coin(35)) return uint8_t(GX_NONE);
    if (!allowIdx || rng.coin(45)) return uint8_t(GX_DIRECT);
    return uint8_t(rng.coin() ? GX_INDEX8 : GX_INDEX16);
  };
  const uint8_t comps[5] = {GX_U8, GX_S8, GX_U16, GX_S16, GX_F32};
  if (a == GX_VA_PNMTXIDX) {
    s.type = rng.coin(50) ? GX_DIRECT : GX_NONE;
    s.comp = GX_U8, s.cnt = 1;
  } else if (a >= GX_VA_TEX0MTXIDX && a <= GX_VA_TEX7MTXIDX) {
    s.type = rng.coin(25) ? GX_DIRECT : GX_NONE;
    s.comp = GX_U8, s.cnt = 1;
  } else if (a == GX_VA_POS) {
    s.type = pickType(false, true);
    s.comp = comps[rng.below(5)];
    s.cnt = rng.coin() ? 3 : 2;
    s.frac = rng.below(18);
  } else if (a == GX_VA_NRM) {
    s.type = pickType(true, true);
    s.comp = comps[rng.below(5)];
    s.cnt = rng.coin(70) ? 3 : 9;
    s.nbt3 = s.cnt == 9 && rng.coin(50);
    s.frac = rng.below(16);
  } else if (is_clr(a)) {
    s.type = pickType(true, true);
    s.comp = rng.below(6); // GX_RGB565..GX_RGBA8
    s.cnt = 1;
  } else {
    s.type = pickType(true, true);
    s.comp = comps[rng.below(5)];
    s.cnt = rng.coin() ? 2 : 1;
    s.frac = rng.below(18);
  }
  if (linesOnlyBasic && a != GX_VA_POS && a != GX_VA_PNMTXIDX && a != GX_VA_CLR0 && a != GX_VA_TEX0) {
    s = {};
  }
  s.le = rng.coin(30);
  if (s.type == GX_INDEX8 || s.type == GX_INDEX16) {
    uint32_t eb = ref_elem_bytes(a, s);
    if (a == GX_VA_NRM && s.cnt == 9 && s.nbt3) eb = ref_comp_size(s.comp) * 3;
    s.arrayStride = static_cast<uint8_t>(eb + rng.below(4)); // sometimes padded
  }
}

static void build_case(Case& c, Rng& rng, uint32_t vtxCount, bool linesOnlyBasic, uint8_t lineMode) {
  c.vtxCount = vtxCount;
  ShaderConfig& sc = c.config.shaderConfig;
  sc = {};
  sc.lineMode = lineMode;
  uint32_t off = 0;
  for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
    Spec& s = c.spec[a];
    gen_spec(rng, a, s, linesOnlyBasic);
    AttrConfig& m = sc.attrs[a];
    m = {};
    if (s.type == GX_NONE) continue;
    m.attrType = s.type;
    m.cnt = s.cnt;
    m.compType = s.comp;
    m.offset = static_cast<uint8_t>(off);
    m.frac = s.frac;
    m.nbt3 = s.nbt3;
    m.stride = s.arrayStride;
    m.le = s.type == GX_DIRECT ? false : s.le;
    if (s.type == GX_DIRECT) {
      off += ref_elem_bytes(a, s);
    } else if (s.type == GX_INDEX8) {
      off += s.nbt3 ? 3 : 1;
    } else {
      off += s.nbt3 ? 6 : 2;
    }
  }
  sc.vtxStride = static_cast<uint8_t>(off);
  c.vtxStride = off;

  // Arrays (snapshotted into frame.storage at odd offsets) and the raw vertex stream.
  c.frame.storage.assign(1 + rng.below(7), 0xEE);
  for (int a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
    const Spec& s = c.spec[a];
    if (s.type != GX_INDEX8 && s.type != GX_INDEX16) continue;
    c.arrayElems[a] = 1 + rng.below(s.type == GX_INDEX8 ? 40 : 300);
    c.arrayBase[a] = static_cast<uint32_t>(c.frame.storage.size());
    const size_t bytes = size_t(c.arrayElems[a]) * s.arrayStride + 4;
    for (size_t i = 0; i < bytes; ++i) c.frame.storage.push_back(static_cast<uint8_t>(rng.next() >> 17));
    c.draw.immediateData.arrayStart[a - GX_VA_POS] = c.arrayBase[a];
    // an extra stretch of other data right behind (arrays are packed back to back in the real snapshot buffer)
  }
  c.draw.immediateData.currentPnMtx = rng.below(10);

  const uint32_t pad = rng.below(9) * 4;
  c.frame.verts.assign(pad, 0xCD);
  for (uint32_t v = 0; v < vtxCount; ++v) {
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
      const Spec& s = c.spec[a];
      if (s.type == GX_NONE) continue;
      if (s.type == GX_DIRECT) {
        const uint32_t eb = ref_elem_bytes(a, s);
        for (uint32_t i = 0; i < eb; ++i) c.frame.verts.push_back(static_cast<uint8_t>(rng.next() >> 23));
      } else {
        const int n = s.nbt3 ? 3 : 1;
        for (int k = 0; k < n; ++k) {
          const uint32_t idx = rng.below(c.arrayElems[a] + (rng.coin(10) ? 5000 : 1));
          if (s.type == GX_INDEX8) {
            c.frame.verts.push_back(static_cast<uint8_t>(idx));
          } else {
            c.frame.verts.push_back(static_cast<uint8_t>(idx >> 8));
            c.frame.verts.push_back(static_cast<uint8_t>(idx));
          }
        }
      }
    }
  }
  c.frame.verts.insert(c.frame.verts.end(), 8, 0xAB);
  c.draw.vertRange = {pad, vtxCount * c.vtxStride};
  c.draw.vtxCount = vtxCount;
  c.draw.instanceCount = 1;
}

struct RefAttr {
  GXAttr attr;
  DecodedAttr::Kind kind;
  uint8_t comps;
  const char* name;
};

// Layout expected from the contract text (independent of decoded_layout()).
static std::vector<RefAttr> ref_layout(const Case& c, bool withExtras, uint8_t lineMode) {
  std::vector<RefAttr> l;
  using K = DecodedAttr::Kind;
  l.push_back({GX_VA_PNMTXIDX, K::Float, 1, "aPnMtxIdx"});
  for (int i = 0; i < 8; ++i) {
    static const char* n[8] = {"aTexMtxIdx0", "aTexMtxIdx1", "aTexMtxIdx2", "aTexMtxIdx3",
                               "aTexMtxIdx4", "aTexMtxIdx5", "aTexMtxIdx6", "aTexMtxIdx7"};
    if (c.spec[GX_VA_TEX0MTXIDX + i].type != GX_NONE) l.push_back({GXAttr(GX_VA_TEX0MTXIDX + i), K::Float, 1, n[i]});
  }
  if (c.spec[GX_VA_POS].type != GX_NONE) l.push_back({GX_VA_POS, K::Float, 3, "aPos"});
  if (c.spec[GX_VA_NRM].type != GX_NONE) {
    l.push_back({GX_VA_NRM, K::Float, 3, "aNrm"});
    if (c.spec[GX_VA_NRM].cnt == 9) {
      l.push_back({GX_VA_NRM, K::Float, 3, "aBin"});
      l.push_back({GX_VA_NRM, K::Float, 3, "aTan"});
    }
  }
  if (c.spec[GX_VA_CLR0].type != GX_NONE) l.push_back({GX_VA_CLR0, K::UByte4Norm, 4, "aClr0"});
  if (c.spec[GX_VA_CLR1].type != GX_NONE) l.push_back({GX_VA_CLR1, K::UByte4Norm, 4, "aClr1"});
  for (int i = 0; i < 8; ++i) {
    static const char* n[8] = {"aTex0", "aTex1", "aTex2", "aTex3", "aTex4", "aTex5", "aTex6", "aTex7"};
    if (c.spec[GX_VA_TEX0 + i].type != GX_NONE) l.push_back({GXAttr(GX_VA_TEX0 + i), K::Float, 2, n[i]});
  }
  if (withExtras && lineMode != 0) {
    l.push_back({GX_VA_NULL, K::Float, 1, "aCorner"});
    if (lineMode != 3) {
      l.push_back({GX_VA_NULL, K::Float, 3, "aPosOther"});
      l.push_back({GX_VA_NULL, K::Float, 1, "aPnMtxIdxOther"});
    }
  }
  return l;
}

// Reference decode of one vertex into `dst` (decoded layout `l`).
static void ref_decode_vertex(const Case& c, const std::vector<RefAttr>& l, const std::vector<uint32_t>& offs,
                              uint32_t v, uint8_t* dst) {
  const uint8_t* vtx = c.frame.verts.data() + c.draw.vertRange.offset + size_t(v) * c.vtxStride;
  uint32_t nrmSlice = 0;
  for (size_t k = 0; k < l.size(); ++k) {
    uint8_t* d = dst + offs[k];
    const int a = l[k].attr;
    if (a == GX_VA_NULL) continue;
    const Spec& s = c.spec[a];
    uint32_t slice = 0;
    if (a == GX_VA_NRM) slice = nrmSlice++;
    if (s.type == GX_NONE) {
      // only PNMTXIDX can be missing from the layout
      const float f = static_cast<float>(c.draw.immediateData.currentPnMtx);
      std::memcpy(d, &f, 4);
      continue;
    }
    const uint32_t cs = is_clr(a) || is_mtx(a) ? 1 : ref_comp_size(s.comp);
    const uint8_t* src = nullptr;
    uint32_t within = 0;
    const AttrConfig& m = c.config.shaderConfig.attrs[a];
    if (a == GX_VA_NRM && s.cnt == 9 && !(s.nbt3 && s.type != GX_DIRECT)) within = slice * 3 * cs;
    bool ok = true;
    bool littleEndian = false; // data endianness
    if (s.type == GX_DIRECT) {
      src = vtx + m.offset + within;
    } else {
      const uint32_t idxBytes = s.type == GX_INDEX8 ? 1 : 2;
      const uint32_t idxOff = m.offset + (s.nbt3 ? slice * idxBytes : 0);
      const uint32_t idx = ref_uint(vtx + idxOff, idxBytes, false);
      const uint64_t addr = uint64_t(c.arrayBase[a]) + uint64_t(idx) * s.arrayStride + within;
      // an NBT slice reads three components, not the whole nine-component element
      const uint32_t eb = (a == GX_VA_NRM && s.cnt == 9) ? 3 * cs : ref_elem_bytes(a, s);
      if (addr + eb > c.frame.storage.size()) {
        ok = false;
      } else {
        src = c.frame.storage.data() + addr;
      }
      littleEndian = s.le;
    }
    if (!ok) {
      std::memset(d, 0, l[k].kind == DecodedAttr::Kind::Float ? l[k].comps * 4 : 4);
      continue;
    }
    if (a == GX_VA_PNMTXIDX || is_mtx(a)) {
      float f = a == GX_VA_PNMTXIDX ? static_cast<float>(src[0] / 3) : static_cast<float>(src[0]);
      std::memcpy(d, &f, 4);
    } else if (is_clr(a)) {
      ref_color(src, s.comp, littleEndian, d);
    } else {
      const int in = a == GX_VA_NRM ? 3 : s.cnt;
      float f[3] = {0, 0, 0};
      for (int i = 0; i < in; ++i) f[i] = ref_float(src + i * cs, s.comp, littleEndian, s.frac);
      std::memcpy(d, f, l[k].comps * 4);
    }
  }
}

static std::vector<uint32_t> ref_offsets(const std::vector<RefAttr>& l, uint32_t& stride) {
  std::vector<uint32_t> o;
  stride = 0;
  for (auto& r : l) {
    o.push_back(stride);
    stride += r.kind == DecodedAttr::Kind::Float ? r.comps * 4 : 4;
  }
  return o;
}

static void check_layout(const DecodedLayout& dl, const std::vector<RefAttr>& l, const std::vector<uint32_t>& offs,
                         uint32_t stride, const char* what) {
  CHECK_T(dl.count == l.size(), "%s: count %u vs %zu", what, dl.count, l.size());
  CHECK_T(dl.stride == stride, "%s: stride %u vs %u", what, dl.stride, stride);
  for (size_t i = 0; i < l.size() && i < dl.count; ++i) {
    CHECK_T(dl.attrs[i].attr == l[i].attr && dl.attrs[i].kind == l[i].kind && dl.attrs[i].components == l[i].comps &&
                dl.attrs[i].offset == offs[i] && std::strcmp(dl.attrs[i].name, l[i].name) == 0,
            "%s: entry %zu (%s) attr %d kind %d comps %u off %u name %s", what, i, l[i].name, int(dl.attrs[i].attr),
            int(dl.attrs[i].kind), dl.attrs[i].components, dl.attrs[i].offset, dl.attrs[i].name);
  }
}

static void test_random_formats(uint32_t seeds) {
  uint32_t draws = 0;
  for (uint32_t seed = 1; seed <= seeds; ++seed) {
    Rng rng(seed);
    Case c;
    const uint32_t n = 1 + rng.below(40);
    build_case(c, rng, n, false, 0);
    const auto l = ref_layout(c, false, 0);
    uint32_t stride;
    const auto offs = ref_offsets(l, stride);
    const DecodedLayout dl = decoded_layout(c.config.shaderConfig);
    check_layout(dl, l, offs, stride, "random");
    if (dl.stride != stride || dl.count != l.size()) continue;

    std::vector<uint8_t> out(7, 0x55); // stale garbage must not leak
    decode_draw(c.config, c.draw, c.frame, dl, out);
    CHECK_T(out.size() == size_t(n) * stride, "seed %u: out size %zu vs %zu", seed, out.size(), size_t(n) * stride);
    std::vector<uint8_t> ref(size_t(n) * stride);
    for (uint32_t v = 0; v < n; ++v) ref_decode_vertex(c, l, offs, v, ref.data() + size_t(v) * stride);
    if (out.size() == ref.size() && std::memcmp(out.data(), ref.data(), ref.size()) != 0) {
      size_t i = 0;
      while (out[i] == ref[i]) ++i;
      const uint32_t v = i / stride, o = i % stride;
      size_t k = 0;
      while (k + 1 < l.size() && offs[k + 1] <= o) ++k;
      std::printf("seed %u: mismatch vertex %u byte %u (attr %s, spec type %d comp %d cnt %d frac %d nbt3 %d le %d): "
                  "got %02x want %02x\n",
                  seed, v, o, l[k].name, c.spec[l[k].attr].type, c.spec[l[k].attr].comp, c.spec[l[k].attr].cnt,
                  c.spec[l[k].attr].frac, c.spec[l[k].attr].nbt3, c.spec[l[k].attr].le, out[i], ref[i]);
      ++gFailures;
    }
    ++gChecks;
    ++draws;
  }
  std::printf("random formats: %u draws compared\n", draws);
}

// Every attribute type in isolation, every combination, so that a regression names the culprit.
static void test_exhaustive_single(uint32_t reps) {
  uint32_t combos = 0;
  const uint8_t comps[5] = {GX_U8, GX_S8, GX_U16, GX_S16, GX_F32};
  for (int mode = 0; mode < 3; ++mode) {
    const uint8_t type = mode == 0 ? GX_DIRECT : mode == 1 ? GX_INDEX8 : GX_INDEX16;
    for (int le = 0; le < (mode ? 2 : 1); ++le) {
      // floats
      for (int attr : {int(GX_VA_POS), int(GX_VA_NRM), int(GX_VA_TEX0)}) {
        for (uint8_t comp : comps) {
          for (uint8_t cnt : attr == GX_VA_POS ? std::vector<uint8_t>{2, 3}
                              : attr == GX_VA_NRM ? std::vector<uint8_t>{3, 9}
                                                  : std::vector<uint8_t>{1, 2}) {
            for (int nbt3 = 0; nbt3 < (cnt == 9 ? 2 : 1); ++nbt3) {
              for (uint32_t rep = 0; rep < reps; ++rep) {
                Rng rng(1000 + combos * 31 + rep);
                Case c;
                // start from an empty config and add the one attribute (+ POS so there is always geometry)
                c.vtxCount = 1 + rng.below(20);
                ShaderConfig& sc = c.config.shaderConfig;
                sc = {};
                uint32_t off = 0;
                for (int a : {int(GX_VA_POS), attr}) {
                  if (a == GX_VA_POS && attr == GX_VA_POS) a = GX_VA_POS;
                  Spec& s = c.spec[a];
                  s = {};
                  s.type = (a == attr) ? type : GX_DIRECT;
                  s.comp = comp;
                  s.cnt = a == attr ? cnt : 3;
                  s.frac = rng.below(16);
                  s.le = le;
                  s.nbt3 = nbt3;
                  if (a == GX_VA_POS && attr != GX_VA_POS) {
                    s.comp = GX_F32;
                    s.nbt3 = false;
                  }
                  uint32_t eb = ref_elem_bytes(a, s);
                  if (a == GX_VA_NRM && s.cnt == 9 && s.nbt3) eb = ref_comp_size(s.comp) * 3;
                  s.arrayStride = static_cast<uint8_t>(eb);
                  AttrConfig& m = sc.attrs[a];
                  m = {};
                  m.attrType = s.type;
                  m.cnt = s.cnt;
                  m.compType = s.comp;
                  m.offset = static_cast<uint8_t>(off);
                  m.frac = s.frac;
                  m.nbt3 = s.nbt3;
                  m.stride = s.arrayStride;
                  m.le = s.type == GX_DIRECT ? false : s.le;
                  off += s.type == GX_DIRECT ? ref_elem_bytes(a, s) : (s.type == GX_INDEX8 ? (s.nbt3 ? 3 : 1) : (s.nbt3 ? 6 : 2));
                  if (a == attr && attr == GX_VA_POS) break;
                }
                sc.vtxStride = static_cast<uint8_t>(off);
                c.vtxStride = off;
                c.frame.storage.assign(3, 0xEE);
                const int a = attr;
                const Spec& s = c.spec[a];
                if (s.type != GX_DIRECT) {
                  c.arrayElems[a] = 8;
                  c.arrayBase[a] = 3;
                  for (size_t i = 0; i < size_t(8) * s.arrayStride + 4; ++i) c.frame.storage.push_back(uint8_t(rng.next() >> 11));
                  c.draw.immediateData.arrayStart[a - GX_VA_POS] = 3;
                }
                c.frame.verts.clear();
                for (uint32_t v = 0; v < c.vtxCount; ++v) {
                  for (int aa = GX_VA_POS; aa <= GX_VA_TEX7; ++aa) {
                    const Spec& ss = c.spec[aa];
                    if (ss.type == GX_NONE) continue;
                    if (ss.type == GX_DIRECT) {
                      for (uint32_t i = 0; i < ref_elem_bytes(aa, ss); ++i) c.frame.verts.push_back(uint8_t(rng.next() >> 19));
                    } else {
                      for (int k = 0; k < (ss.nbt3 ? 3 : 1); ++k) {
                        const uint32_t idx = rng.below(10); // 8, 9 are out of range
                        if (ss.type == GX_INDEX8) c.frame.verts.push_back(uint8_t(idx));
                        else { c.frame.verts.push_back(0); c.frame.verts.push_back(uint8_t(idx)); }
                      }
                    }
                  }
                }
                c.draw.vertRange = {0, c.vtxCount * c.vtxStride};
                c.draw.vtxCount = c.vtxCount;
                const auto l = ref_layout(c, false, 0);
                uint32_t stride;
                const auto offs = ref_offsets(l, stride);
                const DecodedLayout dl = decoded_layout(sc);
                check_layout(dl, l, offs, stride, "single");
                std::vector<uint8_t> out;
                decode_draw(c.config, c.draw, c.frame, dl, out);
                std::vector<uint8_t> ref(size_t(c.vtxCount) * stride);
                for (uint32_t v = 0; v < c.vtxCount; ++v) ref_decode_vertex(c, l, offs, v, ref.data() + size_t(v) * stride);
                CHECK_T(out == ref, "single: mode %d le %d attr %d comp %d cnt %d nbt3 %d", mode, le, attr, comp, cnt, nbt3);
                ++combos;
              }
            }
          }
        }
      }
      // colors
      for (uint8_t fmt = 0; fmt < 6; ++fmt) {
        for (uint32_t rep = 0; rep < reps; ++rep) {
          Rng rng(5000 + combos * 17 + rep);
          Case c;
          c.vtxCount = 1 + rng.below(20);
          ShaderConfig& sc = c.config.shaderConfig;
          sc = {};
          uint32_t off = 0;
          for (int a : {int(GX_VA_POS), int(GX_VA_CLR1)}) {
            Spec& s = c.spec[a];
            s = {};
            s.type = a == GX_VA_POS ? GX_DIRECT : type;
            s.comp = a == GX_VA_POS ? GX_F32 : fmt;
            s.cnt = a == GX_VA_POS ? 3 : 1;
            s.le = le;
            s.arrayStride = static_cast<uint8_t>(ref_elem_bytes(a, s));
            AttrConfig& m = sc.attrs[a];
            m = {};
            m.attrType = s.type; m.cnt = s.cnt; m.compType = s.comp; m.offset = static_cast<uint8_t>(off);
            m.stride = s.arrayStride; m.le = s.type == GX_DIRECT ? false : s.le;
            off += s.type == GX_DIRECT ? ref_elem_bytes(a, s) : (s.type == GX_INDEX8 ? 1 : 2);
          }
          sc.vtxStride = static_cast<uint8_t>(off);
          c.vtxStride = off;
          c.frame.storage.assign(1, 0);
          const int a = GX_VA_CLR1;
          if (type != GX_DIRECT) {
            c.arrayElems[a] = 8; c.arrayBase[a] = 1;
            for (size_t i = 0; i < size_t(8) * c.spec[a].arrayStride + 4; ++i) c.frame.storage.push_back(uint8_t(rng.next() >> 11));
            c.draw.immediateData.arrayStart[a - GX_VA_POS] = 1;
          }
          for (uint32_t v = 0; v < c.vtxCount; ++v) {
            for (int aa : {int(GX_VA_POS), int(GX_VA_CLR1)}) {
              const Spec& ss = c.spec[aa];
              if (ss.type == GX_DIRECT) {
                for (uint32_t i = 0; i < ref_elem_bytes(aa, ss); ++i) c.frame.verts.push_back(uint8_t(rng.next() >> 19));
              } else {
                const uint32_t idx = rng.below(10);
                if (ss.type == GX_INDEX8) c.frame.verts.push_back(uint8_t(idx));
                else { c.frame.verts.push_back(0); c.frame.verts.push_back(uint8_t(idx)); }
              }
            }
          }
          c.draw.vertRange = {0, c.vtxCount * c.vtxStride};
          c.draw.vtxCount = c.vtxCount;
          const auto l = ref_layout(c, false, 0);
          uint32_t stride;
          const auto offs = ref_offsets(l, stride);
          const DecodedLayout dl = decoded_layout(sc);
          std::vector<uint8_t> out;
          decode_draw(c.config, c.draw, c.frame, dl, out);
          std::vector<uint8_t> ref(size_t(c.vtxCount) * stride);
          for (uint32_t v = 0; v < c.vtxCount; ++v) ref_decode_vertex(c, l, offs, v, ref.data() + size_t(v) * stride);
          CHECK_T(out == ref, "single color: mode %d le %d fmt %d", mode, le, fmt);
          ++combos;
        }
      }
    }
  }
  std::printf("exhaustive single-attribute combos: %u\n", combos);
}

// ---- directed tests with known numeric values -----------------------------------------------------

static void put_be32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
static void put_be16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
static void put_f32_be(std::vector<uint8_t>& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put_be32(b, u);
}
static float getf(const std::vector<uint8_t>& b, size_t off) {
  float f;
  std::memcpy(&f, b.data() + off, 4);
  return f;
}

static void set_attr(ShaderConfig& sc, GXAttr a, uint8_t type, uint8_t comp, uint8_t cnt, uint8_t offset, uint8_t frac = 0,
                     uint8_t arrayStride = 0, bool le = false, bool nbt3 = false) {
  AttrConfig& m = sc.attrs[a];
  m = {};
  m.attrType = type; m.compType = comp; m.cnt = cnt; m.offset = offset; m.frac = frac; m.stride = arrayStride;
  m.le = le; m.nbt3 = nbt3;
}

static void test_known_values() {
  // pos: S16 x3 with 8 fractional bits (direct), clr0: RGB565 direct, tex0: U8 x2 frac 7 (direct), no matrix index
  Case c;
  ShaderConfig& sc = c.config.shaderConfig;
  sc = {};
  set_attr(sc, GX_VA_POS, GX_DIRECT, GX_S16, 3, 0, 8);
  set_attr(sc, GX_VA_CLR0, GX_DIRECT, GX_RGB565, 1, 6);
  set_attr(sc, GX_VA_TEX0, GX_DIRECT, GX_U8, 2, 8, 7);
  sc.vtxStride = 10;
  c.draw.immediateData.currentPnMtx = 4;
  auto& v = c.frame.verts;
  put_be16(v, 0x0100); put_be16(v, 0xFF00); put_be16(v, 0x0080); // 1.0, -1.0, 0.5
  put_be16(v, 0xF800); // pure red 565
  v.push_back(128); v.push_back(64); // 1.0, 0.5
  put_be16(v, 0x0000); put_be16(v, 0x7FFF); put_be16(v, 0x8000); // 0, 127.99..., -128
  put_be16(v, 0x07E0); // pure green
  v.push_back(0); v.push_back(255);
  c.draw.vertRange = {0, 20};
  c.draw.vtxCount = 2;
  const DecodedLayout dl = decoded_layout(sc);
  CHECK_T(dl.count == 4 && dl.stride == 4 + 12 + 4 + 8, "known: layout count %u stride %u", dl.count, dl.stride);
  CHECK_T(std::strcmp(dl.attrs[0].name, "aPnMtxIdx") == 0 && dl.attrs[0].offset == 0, "known: pnmtx entry");
  std::vector<uint8_t> out;
  decode_draw(c.config, c.draw, c.frame, dl, out);
  CHECK_T(out.size() == 2u * dl.stride, "known: size");
  const uint32_t s = dl.stride;
  CHECK_T(getf(out, 0) == 4.f && getf(out, s) == 4.f, "known: current pn matrix");
  CHECK_T(getf(out, 4) == 1.f && getf(out, 8) == -1.f && getf(out, 12) == 0.5f, "known: pos0 %f %f %f", getf(out, 4), getf(out, 8), getf(out, 12));
  CHECK_T(getf(out, s + 4) == 0.f && getf(out, s + 8) == 127.99609375f && getf(out, s + 12) == -128.f, "known: pos1");
  CHECK_T(out[16] == 255 && out[17] == 0 && out[18] == 0 && out[19] == 255, "known: red %d %d %d %d", out[16], out[17], out[18], out[19]);
  CHECK_T(out[s + 16] == 0 && out[s + 17] == 255 && out[s + 18] == 0 && out[s + 19] == 255, "known: green");
  CHECK_T(getf(out, 20) == 1.f && getf(out, 24) == 0.5f, "known: tex0");
  CHECK_T(getf(out, s + 20) == 0.f && getf(out, s + 24) == 255.f / 128.f, "known: tex1");

  // INDEX16 position array, little-endian array of floats, PNMTXIDX direct (raw 6 -> matrix 2)
  Case d;
  ShaderConfig& sd = d.config.shaderConfig;
  sd = {};
  set_attr(sd, GX_VA_PNMTXIDX, GX_DIRECT, GX_U8, 1, 0);
  set_attr(sd, GX_VA_POS, GX_INDEX16, GX_F32, 3, 1, 0, 12, true);
  sd.vtxStride = 3;
  d.frame.storage.assign(5, 0);
  auto pushle = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; ++i) d.frame.storage.push_back(uint8_t(u >> (8 * i))); };
  for (float f : {1.f, 2.f, 3.f, -4.f, 5.f, -6.f}) pushle(f);
  d.draw.immediateData.arrayStart[0] = 5;
  d.frame.verts = {6, 0, 1, 9, 0, 0, 255, 255, 0}; // pnmtx 6 idx 1; pnmtx 9 idx 0; pnmtx 255 idx 0xFF00 (out of range)
  d.draw.vertRange = {0, 9};
  d.draw.vtxCount = 3;
  const DecodedLayout dd = decoded_layout(sd);
  std::vector<uint8_t> o2;
  decode_draw(d.config, d.draw, d.frame, dd, o2);
  CHECK_T(getf(o2, 0) == 2.f && getf(o2, 4) == -4.f && getf(o2, 8) == 5.f && getf(o2, 12) == -6.f, "known: indexed LE floats");
  CHECK_T(getf(o2, 16) == 3.f && getf(o2, 20) == 1.f && getf(o2, 24) == 2.f && getf(o2, 28) == 3.f, "known: indexed idx0");
  CHECK_T(getf(o2, 32) == 85.f && getf(o2, 36) == 0.f && getf(o2, 40) == 0.f && getf(o2, 44) == 0.f, "known: out-of-range index -> zero (pnmtx 255/3 = 85)");
}

static void test_robustness() {
  // Truncated vertex data: vtxCount larger than what is recorded; trailing vertices decode to zeros.
  Case c;
  ShaderConfig& sc = c.config.shaderConfig;
  sc = {};
  set_attr(sc, GX_VA_POS, GX_DIRECT, GX_F32, 3, 0);
  sc.vtxStride = 12;
  put_f32_be(c.frame.verts, 1.f); put_f32_be(c.frame.verts, 2.f); put_f32_be(c.frame.verts, 3.f);
  put_f32_be(c.frame.verts, 4.f); put_f32_be(c.frame.verts, 5.f); put_f32_be(c.frame.verts, 6.f);
  put_f32_be(c.frame.verts, 7.f); // half a vertex at the end
  c.draw.vertRange = {0, 36};
  c.draw.vtxCount = 3 + 1; // claims 4 vertices (2 are really there)
  c.draw.immediateData.currentPnMtx = 2;
  const DecodedLayout dl = decoded_layout(sc);
  std::vector<uint8_t> out(1000, 0x77);
  decode_draw(c.config, c.draw, c.frame, dl, out);
  CHECK_T(out.size() == 4u * 16, "trunc: size %zu", out.size());
  CHECK_T(getf(out, 4) == 1.f && getf(out, 16 + 4) == 4.f, "trunc: first vertices");
  bool zeros = true;
  for (size_t i = 32; i < 64; ++i) zeros = zeros && out[i] == 0;
  CHECK_T(zeros, "trunc: missing vertices must be zero");

  // vertRange beyond the buffer, zero vertices, vtxStride 0
  c.draw.vertRange = {10000, 36};
  decode_draw(c.config, c.draw, c.frame, dl, out);
  zeros = out.size() == 64;
  for (uint8_t b : out) zeros = zeros && b == 0;
  CHECK_T(zeros, "range past buffer");
  c.draw.vtxCount = 0;
  decode_draw(c.config, c.draw, c.frame, dl, out);
  CHECK_T(out.empty(), "zero vertices");

  // Attribute that does not fit into the declared vertex stride must not read out of bounds.
  Case e;
  ShaderConfig& se = e.config.shaderConfig;
  se = {};
  set_attr(se, GX_VA_POS, GX_DIRECT, GX_F32, 3, 10); // offset 10 + 12 > stride 12
  se.vtxStride = 12;
  e.frame.verts.assign(12 * 3, 0x41);
  e.draw.vertRange = {0, 36};
  e.draw.vtxCount = 3;
  const DecodedLayout de = decoded_layout(se);
  std::vector<uint8_t> o2;
  decode_draw(e.config, e.draw, e.frame, de, o2);
  zeros = true;
  for (size_t i = 4; i < 16; ++i) zeros = zeros && o2[i] == 0;
  CHECK_T(zeros && o2.size() == 3u * 16, "overrunning attribute -> zero");

  // Index pointing into an array snapshot that is shorter than the element.
  Case f;
  ShaderConfig& sf = f.config.shaderConfig;
  sf = {};
  set_attr(sf, GX_VA_POS, GX_INDEX8, GX_F32, 3, 0, 0, 12, false);
  sf.vtxStride = 1;
  f.frame.storage.assign(20, 0x40); // one full element + 8 bytes
  f.draw.immediateData.arrayStart[0] = 0;
  f.frame.verts = {0, 1, 2, 200};
  f.draw.vertRange = {0, 4};
  f.draw.vtxCount = 4;
  const DecodedLayout df = decoded_layout(sf);
  std::vector<uint8_t> o3;
  decode_draw(f.config, f.draw, f.frame, df, o3);
  CHECK_T(o3.size() == 4u * 16 && getf(o3, 4) != 0.f && getf(o3, 16 + 4) == 0.f && getf(o3, 32 + 4) == 0.f && getf(o3, 48 + 4) == 0.f,
          "short array snapshot: elem0 ok, partial element zero, far index zero");

  // Layout cap: more attributes than MaxDecodedAttrs must not overflow.
  ShaderConfig big{};
  for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) set_attr(big, GXAttr(a), GX_DIRECT, GX_F32, 2, 0);
  big.attrs[GX_VA_NRM].cnt = 9;
  big.lineMode = 1;
  const DecodedLayout dbig = decoded_layout(big);
  CHECK_T(dbig.count <= MaxDecodedAttrs, "layout cap");
  std::printf("note: worst-case layout would need %u entries, MaxDecodedAttrs = %zu (count capped at %u)\n", 1 + 8 + 1 + 3 + 2 + 8 + 3,
              MaxDecodedAttrs, dbig.count);
}

// ---- lines / points ------------------------------------------------------------------------------

static void test_lines(uint32_t mode, uint32_t vtxCount, uint64_t seed) {
  Rng rng(seed);
  Case c;
  build_case(c, rng, vtxCount, true, uint8_t(mode));
  c.draw.instanceCount = mode == 1 ? vtxCount / 2 : mode == 2 ? (vtxCount ? vtxCount - 1 : 0) : vtxCount;
  const auto l = ref_layout(c, true, uint8_t(mode));
  uint32_t stride;
  const auto offs = ref_offsets(l, stride);
  const DecodedLayout dl = decoded_layout(c.config.shaderConfig);
  check_layout(dl, l, offs, stride, "lines");
  if (dl.stride != stride) return;
  std::vector<ExpandedPrimitives> chunks;
  expand_lines(c.config, c.draw, c.frame, dl, chunks);

  const uint32_t segs = mode == 1 ? vtxCount / 2 : mode == 2 ? (vtxCount >= 2 ? vtxCount - 1 : 0) : vtxCount;
  uint32_t quadsSeen = 0;
  size_t posIdx = 0, pnmIdx = 0, cornerIdx = 0, posOtherIdx = 0, pnmOtherIdx = 0;
  for (size_t i = 0; i < l.size(); ++i) {
    if (l[i].attr == GX_VA_POS) posIdx = i;
    if (l[i].attr == GX_VA_PNMTXIDX) pnmIdx = i;
    if (std::strcmp(l[i].name, "aCorner") == 0) cornerIdx = i;
    if (std::strcmp(l[i].name, "aPosOther") == 0) posOtherIdx = i;
    if (std::strcmp(l[i].name, "aPnMtxIdxOther") == 0) pnmOtherIdx = i;
  }
  // reference decode of the raw vertices (own layout without extras is a prefix of the line layout)
  std::vector<uint8_t> raw(size_t(vtxCount) * stride, 0);
  for (uint32_t v = 0; v < vtxCount; ++v) ref_decode_vertex(c, l, offs, v, raw.data() + size_t(v) * stride);
  bool allOk = true;
  uint32_t expectedChunks = (segs + 12287) / 12288;
  CHECK_T(chunks.size() == expectedChunks, "lines mode %u n %u: %zu chunks, want %u", mode, vtxCount, chunks.size(), expectedChunks);
  for (const auto& ch : chunks) {
    CHECK_T(ch.vertices.size() % (4 * stride) == 0 && ch.vertices.size() / stride <= 49152, "chunk size");
    const uint32_t quads = ch.vertices.size() / stride / 4;
    CHECK_T(ch.indices.size() == size_t(quads) * 6, "chunk indices count");
    for (uint32_t q = 0; q < quads; ++q, ++quadsSeen) {
      const uint32_t a = mode == 1 ? quadsSeen * 2 : quadsSeen;
      const uint32_t b = mode == 3 ? a : a + 1;
      static const uint16_t quadIdx[6] = {0, 1, 2, 2, 1, 3};
      for (int k = 0; k < 6; ++k) allOk = allOk && ch.indices[q * 6 + k] == q * 4 + quadIdx[k];
      for (uint32_t corner = 0; corner < 4; ++corner) {
        const uint8_t* got = ch.vertices.data() + (size_t(q) * 4 + corner) * stride;
        const bool useB = mode != 3 && corner >= 2;
        const uint32_t self = useB ? b : a, other = useB ? a : b;
        // own attributes (everything but the extras)
        const uint32_t ownBytes = offs[cornerIdx];
        allOk = allOk && std::memcmp(got, raw.data() + size_t(self) * stride, ownBytes) == 0;
        float cf;
        std::memcpy(&cf, got + offs[cornerIdx], 4);
        allOk = allOk && cf == float(corner);
        if (mode != 3) {
          allOk = allOk && std::memcmp(got + offs[posOtherIdx], raw.data() + size_t(other) * stride + offs[posIdx], 12) == 0;
          allOk = allOk && std::memcmp(got + offs[pnmOtherIdx], raw.data() + size_t(other) * stride + offs[pnmIdx], 4) == 0;
        }
      }
    }
  }
  CHECK_T(allOk, "lines mode %u n %u seed %" PRIu64 ": quad contents", mode, vtxCount, seed);
  CHECK_T(quadsSeen == segs, "lines mode %u n %u: %u quads, want %u", mode, vtxCount, quadsSeen, segs);
}

// ---- benchmark -------------------------------------------------------------------------------------

static double bench(const char* name, const Case& c, uint32_t reps) {
  const DecodedLayout dl = decoded_layout(c.config.shaderConfig);
  std::vector<uint8_t> out;
  decode_draw(c.config, c.draw, c.frame, dl, out); // warm up
  const auto t0 = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < reps; ++i) decode_draw(c.config, c.draw, c.frame, dl, out);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double mv = double(c.draw.vtxCount) * reps / s / 1e6;
  std::printf("bench %-34s %6.1f Mvert/s (%u B raw, %u B decoded per vertex)\n", name, mv, c.vtxStride, dl.stride);
  return mv;
}

static Case make_bench(bool indexed, uint8_t posComp, uint32_t n) {
  Rng rng(99);
  Case c;
  ShaderConfig& sc = c.config.shaderConfig;
  sc = {};
  uint32_t off = 0;
  const uint8_t t = indexed ? GX_INDEX16 : GX_DIRECT;
  auto add = [&](GXAttr a, uint8_t comp, uint8_t cnt, uint8_t frac, uint8_t type, uint8_t astride) {
    set_attr(sc, a, type, comp, cnt, uint8_t(off), frac, astride, false);
    Spec& s = c.spec[a];
    s.type = type; s.comp = comp; s.cnt = cnt;
    off += type == GX_DIRECT ? (is_clr(a) ? 4 : ref_comp_size(comp) * cnt) : 2;
  };
  add(GX_VA_PNMTXIDX, GX_U8, 1, 0, GX_DIRECT, 0);
  add(GX_VA_POS, posComp, 3, 8, t, uint8_t(ref_comp_size(posComp) * 3));
  add(GX_VA_NRM, GX_S16, 3, 14, t, 6);
  add(GX_VA_CLR0, GX_RGBA8, 1, 0, GX_DIRECT, 0);
  add(GX_VA_TEX0, GX_S16, 2, 8, t, 4);
  sc.vtxStride = uint8_t(off);
  c.vtxStride = off;
  c.vtxCount = n;
  const uint32_t elems = 4000;
  c.frame.storage.assign(0, 0);
  uint32_t base = 0;
  for (int a : {int(GX_VA_POS), int(GX_VA_NRM), int(GX_VA_TEX0)}) {
    if (!indexed) continue;
    c.draw.immediateData.arrayStart[a - GX_VA_POS] = base;
    const size_t bytes = size_t(elems) * sc.attrs[a].stride;
    for (size_t i = 0; i < bytes; ++i) c.frame.storage.push_back(uint8_t(rng.next() >> 20));
    base += uint32_t(bytes);
  }
  for (uint32_t v = 0; v < n; ++v) {
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
      if (c.spec[a].type == GX_NONE) continue;
      if (c.spec[a].type == GX_DIRECT) {
        const uint32_t eb = is_clr(a) ? 4 : ref_comp_size(c.spec[a].comp) * c.spec[a].cnt;
        for (uint32_t i = 0; i < eb; ++i) c.frame.verts.push_back(uint8_t(rng.next() >> 25));
      } else {
        const uint32_t idx = rng.below(elems);
        c.frame.verts.push_back(uint8_t(idx >> 8)); c.frame.verts.push_back(uint8_t(idx));
      }
    }
  }
  c.draw.vertRange = {0, n * c.vtxStride};
  c.draw.vtxCount = n;
  return c;
}

int main(int argc, char** argv) {
  const bool doBench = argc > 1 && std::strcmp(argv[1], "bench") == 0;
  test_known_values();
  test_robustness();
  test_exhaustive_single(6);
  test_random_formats(4000);
  for (uint32_t mode = 1; mode <= 3; ++mode) {
    for (uint32_t n : {0u, 1u, 2u, 3u, 7u, 100u}) test_lines(mode, n, 40 + n * 7 + mode);
    for (uint64_t seed = 1; seed <= 50; ++seed) test_lines(mode, 1 + uint32_t(seed % 17), seed * 977 + mode);
  }
  test_lines(1, 40000, 5);  // 20000 segments -> 2 chunks
  test_lines(2, 25000, 6);  // 24999 segments -> 3 chunks
  test_lines(3, 13000, 7);  // 13000 points -> 2 chunks
  std::printf("%d checks, %d failures\n", gChecks, gFailures);
  if (doBench) {
    bench("direct F32 pos (pn,pos,nrm,clr,tex)", make_bench(false, GX_F32, 100000), 40);
    bench("direct S16 pos", make_bench(false, GX_S16, 100000), 40);
    bench("indexed16 F32 pos", make_bench(true, GX_F32, 100000), 40);
    bench("indexed16 S16 pos", make_bench(true, GX_S16, 100000), 40);
  }
  return gFailures == 0 ? 0 : 1;
}
