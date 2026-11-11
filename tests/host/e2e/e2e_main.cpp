// End-to-end host render harness for the Vita GX renderer.
//
// Runs the REAL pipeline, the same objects the Vita build links, on the host:
//   GX API calls -> FIFO -> command processor -> gfx::vita::Frame -> shader generator + vertex decoder +
//   texture upload -> GL replay (EFB FBO, EFB copies) -> present() -> Mesa llvmpipe (EGL surfaceless, GLES2)
// and checks pixels. The frame lifecycle mirrors vita/aurora/backend.cpp (begin_frame / end_frame).
//
// What this proves: the logic of the pieces together, given standard GLES2 semantics. What it cannot: vitaGL's Cg
// translator, its framebuffer row order, the 128/16 uniform caps, speed (see tests/host/e2e/README.md).
//
// Orientation: images are "top row first" (what a GL window would show). The renderer's assumption
// (kFboRowsFlipped = false) is that its EFB and present() are consistent with GL conventions, so a GX top-left draw
// must appear at the top-left of the presented frame. The `orientation` scene pins that assumption; on a Vita the
// same question depends on how vitaGL orders FBO rows, which only hardware answers.
//
//   run.sh [scene-name ...]        PNGs go to $TPV_E2E_PNG (default /tmp/tpv/e2e/png)
#include <dolphin/gf.h>
#include <dolphin/gx.h>

#include <gfx/recording.hpp>
#include <gfx/vita_frame.hpp>
#include <gfx/vita_texture.hpp>
#include <gx/fifo.hpp>
#include <gx/gx.hpp>
#include <gx/pipeline.hpp>
#include <gx/shader_info.hpp>
#include <gx/texture.hpp>
#include <gx/vita_gx.hpp>

#include "glctx.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr int W = 640;
constexpr int H = 480;
// The GL surface the harness renders to: the largest present size a scene asks for (960x544 is the Vita screen; 640x600
// is taller than the 4:3 EFB, for the letterbox case).
constexpr int kSurfaceW = 960;
constexpr int kSurfaceH = 600;

// ---- image + checker ----------------------------------------------------------------------------------------------
struct Image {
  std::vector<uint8_t> px; // RGBA8, top row first
  int w = W;               // the presented frame: W x H, or the scene's own present size
  int h = H;
  std::array<int, 4> at(int x, int y) const {
    const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  }
};

// What the draws of the last frame needed from the vertex stage: for every distinct pipeline, the decoded attribute list
// (what the replay enables) and the attributes the generated vertex shader declares (what GXM sees as vertex inputs).
struct AttrAudit {
  uint32_t draws = 0;
  uint32_t layoutMax = 0;
  uint32_t declaredMax = 0;
};
AttrAudit gAudit;

struct Checker {
  const char* scene = "";
  int checks = 0;
  int failures = 0;
  void fail(const std::string& what) {
    ++failures;
    if (failures <= kMaxPrinted) { // a broken texture format fails every texel: keep the log readable
      std::printf("  FAIL [%s] %s\n", scene, what.c_str());
    }
  }
  static constexpr int kMaxPrinted = 8;
  // Pixel (x, y) in GX-top-first coordinates must be within `tol` of (r, g, b) on every colour channel.
  bool px(const Image& img, int x, int y, int r, int g, int b, int tol = 3) {
    ++checks;
    const auto c = img.at(x, y);
    if (std::abs(c[0] - r) > tol || std::abs(c[1] - g) > tol || std::abs(c[2] - b) > tol) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "pixel (%d,%d) = (%d,%d,%d), expected (%d,%d,%d) +-%d", x, y, c[0], c[1], c[2], r,
                    g, b, tol);
      fail(buf);
      return false;
    }
    return true;
  }
  const AttrAudit* audit = nullptr; // set by the runner: the draws of this scene's frame
  // The decoded attribute list and the declared vertex-shader attributes of the scene's draws (maximum over its draws)
  // must be exactly the predicted counts, and below the vitaGL / GXM cap of 16 vertex attributes.
  bool attrs(uint32_t layout, uint32_t declared) {
    constexpr uint32_t kVitaAttribs = 16;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "vertex attributes: decoded %u (expected %u), declared %u (expected %u), draws %u",
                  audit->layoutMax, layout, audit->declaredMax, declared, audit->draws);
    return is(audit->draws != 0 && audit->layoutMax == layout && audit->declaredMax == declared &&
                  audit->layoutMax <= kVitaAttribs && audit->declaredMax <= kVitaAttribs,
              buf);
  }
  bool is(bool cond, const std::string& what) {
    ++checks;
    if (!cond) {
      fail(what);
    }
    return cond;
  }
};

constexpr int kGray = 64; // GXInit's clear colour

// Declared vertex attributes of a generated vertex shader: its `attribute <type> <name>;` lines.
uint32_t declared_attributes(const std::string& vertexSource) {
  uint32_t n = 0;
  for (size_t at = vertexSource.find("\nattribute "); at != std::string::npos; at = vertexSource.find("\nattribute ", at + 1)) {
    ++n;
  }
  return n;
}

// Called with the frame recorded and not yet replayed: every draw's pipeline config, as the replay will see it.
void audit_frame() {
  gAudit = {};
  std::vector<aurora::gfx::PipelineRef> seen;
  for (const auto& draw : aurora::gfx::vita::frame().draws) {
    if (std::find(seen.begin(), seen.end(), draw.pipeline) != seen.end()) {
      continue;
    }
    seen.push_back(draw.pipeline);
    const aurora::gx::PipelineConfig* config = aurora::gfx::vita::find_pipeline(draw.pipeline);
    if (config == nullptr) {
      continue;
    }
    const aurora::gx::ShaderInfo info = aurora::gx::build_shader_info(config->shaderConfig);
    const aurora::gx::vita::ProgramSource source = aurora::gx::vita::build_program_source(*config, info);
    ++gAudit.draws;
    gAudit.layoutMax = std::max<uint32_t>(gAudit.layoutMax, source.layout.count);
    gAudit.declaredMax = std::max(gAudit.declaredMax, declared_attributes(source.vertex));
  }
}

// ---- frame driver (mirrors aurora::begin_frame/end_frame in vita/aurora/backend.cpp) ------------------------------
// The frame is presented into a pw x ph window (default: the EFB's own size) and read back as that image.
Image render_frame(const std::function<void()>& draw, int pw = W, int ph = H) {
  aurora::gfx::vita::set_present_size(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
  aurora::gx::fifo::begin_frame();
  aurora::gfx::vita::begin_frame();
  draw();
  aurora::gx::fifo::drain();
  aurora::gx::fifo::end_frame();
  aurora::gx::texture::end_frame();
  audit_frame();
  aurora::gfx::finish();
  aurora::gfx::vita::present();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  Image img;
  img.w = pw;
  img.h = ph;
  img.px.resize(static_cast<size_t>(pw) * ph * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  std::vector<uint8_t> bottomUp(img.px.size());
  glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
  for (int y = 0; y < ph; ++y) {
    std::memcpy(&img.px[static_cast<size_t>(y) * pw * 4], &bottomUp[static_cast<size_t>(ph - 1 - y) * pw * 4], pw * 4);
  }
  return img;
}

void save_png(const std::string& name, const Image& img) {
  const char* dir = std::getenv("TPV_E2E_PNG");
  const std::string base = dir != nullptr ? dir : "/tmp/tpv/e2e/png";
  const std::string cmd = "mkdir -p '" + base + "'";
  if (std::system(cmd.c_str()) != 0) {
    return;
  }
  hostgl::write_png((base + "/" + name + ".png").c_str(), img.px.data(), img.w, img.h);
}

// ---- GX helpers ---------------------------------------------------------------------------------------------------
void ortho(f32 m[4][4], f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
  std::memset(m, 0, sizeof(f32) * 16);
  m[0][0] = 2.f / (r - l);
  m[0][3] = -(r + l) / (r - l);
  m[1][1] = 2.f / (t - b);
  m[1][3] = -(t + b) / (t - b);
  m[2][2] = -1.f / (f - n);
  m[2][3] = -f / (f - n);
  m[3][3] = 1.f;
}

void ident(f32 m[3][4]) {
  std::memset(m, 0, sizeof(f32) * 12);
  m[0][0] = m[1][1] = m[2][2] = 1.f;
}

// Baseline GX state for 2D-style scenes: screen-space ortho (x right, y DOWN, like GX screen space), one TEV stage that
// passes the rasterized colour, vertex colour source, no depth/blend/cull. Vertex format 0 = POS(xyz f32) + CLR0.
void base_state() {
  GXSetViewport(0.f, 0.f, W, H, 0.f, 1.f);
  GXSetScissor(0, 0, W, H);
  f32 proj[4][4];
  ortho(proj, 0.f, H, 0.f, W, 0.1f, 100.f);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  f32 id[3][4];
  ident(id);
  GXLoadPosMtxImm(id, GX_PNMTX0);
  GXLoadNrmMtxImm(id, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);

  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);

  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetCullMode(GX_CULL_NONE);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  // The EFB has no alpha in RGB8_Z24 (GX's default): alpha writes are ignored and copies come out opaque. TP selects
  // RGBA6_Z24 in JFWDisplay::preGX; the alpha scenes opt in to that after calling this.
  GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GXSetDstAlpha(GX_DISABLE, 0);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  // Scenes share one GX state: line / point state is reset here so a scene cannot leak it into the next one.
  GXSetLineWidth(6, GX_TO_ZERO);
  GXSetPointSize(6, GX_TO_ZERO);
  GXEnableTexOffsets(GX_TEXCOORD0, GX_DISABLE, GX_DISABLE);
  GXSetFog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0.f, GXColor{0, 0, 0, 255});
  GXSetFogRangeAdj(GX_DISABLE, 0, nullptr);
  // Indirect texturing off (what J3DSys does before every material): no indirect stages, every TEV stage direct.
  GXSetNumIndStages(0);
  for (const GXTevStageID stage : {GX_TEVSTAGE0, GX_TEVSTAGE1, GX_TEVSTAGE2, GX_TEVSTAGE3}) {
    GXSetTevDirect(stage);
  }
}

struct Vtx {
  f32 x, y, z;
  u8 r, g, b, a;
};

void tri(const Vtx& a, const Vtx& b, const Vtx& c) {
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  for (const Vtx* v : {&a, &b, &c}) {
    GXPosition3f32(v->x, v->y, v->z);
    GXColor4u8(v->r, v->g, v->b, v->a);
  }
  GXEnd();
}

// Axis-aligned rectangle (screen coordinates, y down) at view-space depth -z, solid colour.
void rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, u8 r, u8 g, u8 b, u8 a = 255) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto& [x, y] : {std::pair{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}) {
    GXPosition3f32(x, y, -z);
    GXColor4u8(r, g, b, a);
  }
  GXEnd();
}

// ---- perspective helpers (fog scenes) -----------------------------------------------------------------------------
// GX perspective projection, the C_MTXPerspective convention: the eye looks down -z with y up, NDC z runs from -1 (near)
// to 0 (far), and row 3 carries -1 so that w = -z_eye. Vertical field of view 60 degrees at the harness aspect ratio.
const double kCot = 1.0 / std::tan(30.0 * M_PI / 180.0);
constexpr double kAspect = static_cast<double>(W) / H;

void set_perspective(double n, double f) {
  f32 m[4][4];
  std::memset(m, 0, sizeof(m));
  m[0][0] = static_cast<f32>(kCot / kAspect);
  m[1][1] = static_cast<f32>(kCot);
  m[2][2] = static_cast<f32>(-n / (f - n));
  m[2][3] = static_cast<f32>(-f * n / (f - n));
  m[3][2] = -1.f;
  GXSetProjection(m, GX_PERSPECTIVE);
}

// Rectangle at eye depth d that covers the pixel rectangle [px0,px1] x [py0,py1] (top row first), solid colour.
void persp_rect(double px0, double py0, double px1, double py1, double d, u8 r, u8 g, u8 b) {
  const auto ex = [&](double px) { return static_cast<f32>((px / W * 2.0 - 1.0) * d * kAspect / kCot); };
  const auto ey = [&](double py) { return static_cast<f32>((1.0 - py / H * 2.0) * d / kCot); };
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (const auto& [x, y] : {std::pair{px0, py0}, {px1, py0}, {px1, py1}, {px0, py1}}) {
    GXPosition3f32(ex(x), ey(y), static_cast<f32>(-d));
    GXColor4u8(r, g, b, 255);
  }
  GXEnd();
}

// The fog scenes draw an orange surface under blue fog: colour = lerp(surface, fog, factor).
constexpr GXColor kFogBlue{0, 0, 255, 255};
bool check_fogged(const Image& img, Checker& c, int x, int y, double factor, int tol) {
  return c.px(img, x, y, static_cast<int>(200 * (1 - factor) + 0.5), static_cast<int>(100 * (1 - factor) + 0.5),
              static_cast<int>(255 * factor + 0.5), tol);
}
double linear_fog(double d, double start, double end) { return std::clamp((d - start) / (end - start), 0.0, 1.0); }

// ---- scenes -------------------------------------------------------------------------------------------------------
struct Scene {
  const char* name;
  const char* what;
  std::function<void()> draw;
  std::function<void(const Image&, Checker&)> check;
  std::function<void()> warmup; // optional: drawn (and presented) as an earlier frame, its image discarded
  int presentW = 0; // 0: present into a window of the EFB's own size (W x H)
  int presentH = 0;
};

// Barycentric colour of triangle (a,b,c) at pixel (x, y) (centre sampled), or false when outside.
bool bary(const Vtx& a, const Vtx& b, const Vtx& c, double px, double py, double out[3]) {
  const double d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
  const double l1 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / d;
  const double l2 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / d;
  const double l3 = 1.0 - l1 - l2;
  if (l1 < 0 || l2 < 0 || l3 < 0) {
    return false;
  }
  out[0] = l1 * a.r + l2 * b.r + l3 * c.r;
  out[1] = l1 * a.g + l2 * b.g + l3 * c.g;
  out[2] = l1 * a.b + l2 * b.b + l3 * c.b;
  return true;
}


// ---- scene helpers for textures / arrays / lighting ----------------------------------------------------------------
// 4x4 RGBA8 texture in GX tile layout (one 64-byte tile: 16 AR pairs, then 16 GB pairs; row-major texels).
alignas(32) uint8_t gTexRgba8[64];
std::array<int, 3> test_texel(int x, int y) { return {40 + 60 * x, 40 + 60 * y, ((x ^ y) & 1) ? 220 : 30}; }
void build_test_texture() {
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const auto c = test_texel(x, y);
      const int i = y * 4 + x;
      gTexRgba8[2 * i + 0] = 255;                              // A
      gTexRgba8[2 * i + 1] = static_cast<uint8_t>(c[0]);       // R
      gTexRgba8[32 + 2 * i + 0] = static_cast<uint8_t>(c[1]);  // G
      gTexRgba8[32 + 2 * i + 1] = static_cast<uint8_t>(c[2]);  // B
    }
  }
}

// Switches the baseline state to one textured stage: POS + CLR0 + TEX0 vertices, texture on map 0, given TEV mode.
void textured_state(const void* data, u16 w, u16 h, GXTexFmt fmt, GXTevMode mode, GXTexFilter filter = GX_NEAR) {
  base_state();
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  static GXTexObj obj; // must outlive the FIFO drain
  GXInitTexObj(&obj, data, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&obj, filter, filter, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&obj, GX_TEXMAP0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, mode);
}

// Textured rectangle (texture coordinates cover [0,1]^2), per-vertex colour c.
void tex_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, u8 c = 255) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  const f32 pts[4][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
  for (const auto& p : pts) {
    GXPosition3f32(p[0], p[1], -z);
    GXColor4u8(c, c, c, 255);
    GXTexCoord2f32(p[2], p[3]);
  }
  GXEnd();
}

inline uint32_t be32(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return __builtin_bswap32(u);
}


// Light colours for the light-mask scene; light 2 is deliberately bright and never enabled.
constexpr int kLightColors[8][3] = {{100, 0, 0}, {0, 100, 0}, {255, 255, 255}, {0, 0, 100},
                                    {255, 0, 255}, {100, 100, 0}, {0, 100, 100}, {255, 255, 0}};
constexpr uint32_t kMasks[4] = {(1u << 0) | (1u << 5), 1u << 1, 1u << 3, 1u << 6};

#include "scenes_texture.hpp"
#include "scenes_texobj.hpp"
#include "scenes_indirect.hpp"
#include "scenes_tp_lines.hpp"
#include "scenes_alpha.hpp"
#include "scenes_offscreen.hpp"
#include "scenes_present.hpp"

std::vector<Scene> make_scenes() {
  std::vector<Scene> s;

  s.push_back({"clear", "no draws: the EFB holds GXInit's clear colour everywhere", [] { base_state(); },
               [](const Image& img, Checker& c) {
                 c.px(img, 0, 0, kGray, kGray, kGray);
                 c.px(img, W - 1, H - 1, kGray, kGray, kGray);
                 c.px(img, 320, 240, kGray, kGray, kGray);
               }});

  s.push_back({"orientation", "solid red rect at the GX top-left; pins 'GX row 0 == GL row 0' through EFB and present",
               [] {
                 base_state();
                 rect(0, 0, 100, 100, 1, 255, 0, 0);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 50, 50, 255, 0, 0);
                 c.px(img, 50, H - 50, kGray, kGray, kGray);
                 c.px(img, W - 50, 50, kGray, kGray, kGray);
                 c.px(img, 120, 50, kGray, kGray, kGray);
               }});

  const Vtx A{100, 100, -1, 255, 0, 0, 255}, B{540, 100, -1, 0, 255, 0, 255}, C{320, 420, -1, 0, 0, 255, 255};
  s.push_back({"triangle_vertex_color", "Gouraud triangle: interpolated colours match the analytic barycentric value",
               [=] {
                 base_state();
                 tri(A, B, C);
               },
               [=](const Image& img, Checker& c) {
                 const int pts[][2] = {{320, 207}, {150, 120}, {500, 120}, {320, 400}, {320, 150}};
                 for (const auto& p : pts) {
                   double e[3];
                   if (c.is(bary(A, B, C, p[0] + 0.5, p[1] + 0.5, e), "test point outside triangle")) {
                     c.px(img, p[0], p[1], static_cast<int>(e[0] + 0.5), static_cast<int>(e[1] + 0.5),
                          static_cast<int>(e[2] + 0.5), 6);
                   }
                 }
                 c.px(img, 50, 50, kGray, kGray, kGray);
                 c.px(img, 600, 450, kGray, kGray, kGray);
                 c.px(img, 100, 400, kGray, kGray, kGray);
               }});

  s.push_back({"alpha_blend", "50% blue over red with SRCALPHA/INVSRCALPHA",
               [] {
                 base_state();
                 rect(100, 100, 400, 400, 1, 255, 0, 0);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
                 rect(250, 250, 500, 450, 1, 0, 0, 255, 128);
               },
               [](const Image& img, Checker& c) {
                 // Blue (alpha 128/255) over the gray clear colour and over red.
                 const double a = 128.0 / 255, ia = 127.0 / 255;
                 c.px(img, 150, 150, 255, 0, 0);
                 c.px(img, 300, 300, static_cast<int>(ia * 255 + 0.5), 0, static_cast<int>(a * 255 + 0.5), 3);
                 c.px(img, 450, 430, static_cast<int>(ia * kGray + 0.5), static_cast<int>(ia * kGray + 0.5),
                      static_cast<int>(a * 255 + ia * kGray + 0.5), 3);
                 c.px(img, 50, 50, kGray, kGray, kGray);
               }});

  s.push_back({"depth_test", "LEQUAL depth test: the nearer red quad drawn first survives the farther blue one",
               [] {
                 base_state();
                 GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
                 rect(100, 100, 400, 400, 1, 255, 0, 0); // near
                 rect(250, 250, 500, 450, 5, 0, 0, 255); // far, drawn later, partly behind
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 300, 300, 255, 0, 0); // overlap: red (near) wins
                 c.px(img, 450, 430, 0, 0, 255); // only blue
                 c.px(img, 150, 150, 255, 0, 0);
               }});

  s.push_back({"no_depth_test", "control for depth_test: depth test off, the later (blue) draw wins the overlap",
               [] {
                 base_state();
                 rect(100, 100, 400, 400, 1, 255, 0, 0);
                 rect(250, 250, 500, 450, 5, 0, 0, 255);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 300, 300, 0, 0, 255);
                 c.px(img, 150, 150, 255, 0, 0);
               }});

  s.push_back({"current_matrix", "GXSetCurrentMtx(PNMTX1) with a translated matrix (game: d_home_button.cpp)",
               [] {
                 base_state();
                 f32 t[3][4];
                 ident(t);
                 t[0][3] = 200.f; // +200 px in x
                 GXLoadPosMtxImm(t, GX_PNMTX1);
                 GXSetCurrentMtx(GX_PNMTX1);
                 rect(50, 50, 150, 150, 1, 0, 255, 0); // lands at x 250..350
                 GXSetCurrentMtx(GX_PNMTX0);
                 rect(50, 300, 150, 400, 1, 255, 255, 0); // identity, lands at x 50..150
                 GXSetCurrentMtx(GX_PNMTX1);
                 rect(300, 300, 400, 400, 1, 255, 0, 255); // translated again: x 500..600
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 300, 100, 0, 255, 0);
                 c.px(img, 100, 100, kGray, kGray, kGray); // untranslated position stays empty
                 c.px(img, 100, 350, 255, 255, 0);
                 c.px(img, 550, 350, 255, 0, 255);
                 c.px(img, 350, 350, kGray, kGray, kGray);
               }});

  // ---- textures --------------------------------------------------------------------------------------------------
  s.push_back({"texture_replace", "4x4 RGBA8 texture, NEAR filter, TEV REPLACE: every texel centre matches (incl. row order)",
               [] {
                 build_test_texture();
                 textured_state(gTexRgba8, 4, 4, GX_TF_RGBA8, GX_REPLACE);
                 tex_rect(80, 80, 400, 400, 1);
               },
               [](const Image& img, Checker& c) {
                 for (int ty = 0; ty < 4; ++ty) {
                   for (int tx = 0; tx < 4; ++tx) {
                     const auto t = test_texel(tx, ty);
                     c.px(img, 80 + tx * 80 + 40, 80 + ty * 80 + 40, t[0], t[1], t[2], 2);
                   }
                 }
                 c.px(img, 40, 40, kGray, kGray, kGray);
               }});

  s.push_back({"texture_modulate", "TEV MODULATE: texture x vertex colour (128) is half the texel",
               [] {
                 build_test_texture();
                 textured_state(gTexRgba8, 4, 4, GX_TF_RGBA8, GX_MODULATE);
                 tex_rect(80, 80, 400, 400, 1, 128);
               },
               [](const Image& img, Checker& c) {
                 for (int ty = 0; ty < 4; ty += 3) {
                   for (int tx = 0; tx < 4; tx += 3) {
                     const auto t = test_texel(tx, ty);
                     c.px(img, 80 + tx * 80 + 40, 80 + ty * 80 + 40, t[0] * 128 / 255, t[1] * 128 / 255,
                          t[2] * 128 / 255, 2);
                   }
                 }
               }});

  // ---- lighting --------------------------------------------------------------------------------------------------
  s.push_back({"lighting_diffuse", "one directional light, clamp diffuse: facing, 45 degrees, facing away",
               [] {
                 base_state();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
                 GXLightObj light;
                 GXInitLightColor(&light, GXColor{255, 255, 255, 255});
                 // GXInitLightDir only stores a direction (spot lights); an infinite light is a far position, as
                 // GXInitSpecularDir builds it: travel direction (0,0,-1) x GX_LARGE_NUMBER (-1048576 on GameCube).
                 GXInitLightPos(&light, 0.f, 0.f, 1048576.f);
                 GXLoadLightObjImm(&light, GX_LIGHT0);
                 GXSetChanAmbColor(GX_COLOR0A0, GXColor{30, 30, 30, 255});
                 GXSetChanMatColor(GX_COLOR0A0, GXColor{200, 100, 50, 255});
                 GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
                 const f32 r = 0.70710678f;
                 const f32 normals[3][3] = {{0, 0, 1}, {r, 0, r}, {0, 0, -1}};
                 for (int i = 0; i < 3; ++i) {
                   const f32 x0 = 40.f + i * 200.f;
                   GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                   for (const auto& [x, y] : {std::pair{x0, 100.f}, {x0 + 160, 100.f}, {x0 + 160, 300.f}, {x0, 300.f}}) {
                     GXPosition3f32(x, y, -1.f);
                     GXNormal3f32(normals[i][0], normals[i][1], normals[i][2]);
                   }
                   GXEnd();
                 }
               },
               [](const Image& img, Checker& c) {
                 const double amb = 30.0 / 255;
                 const auto lit = [&](double ndl, int mat) {
                   return static_cast<int>(mat * std::min(1.0, amb + ndl) + 0.5);
                 };
                 c.px(img, 120, 200, lit(1.0, 200), lit(1.0, 100), lit(1.0, 50), 4);
                 c.px(img, 320, 200, lit(0.70710678, 200), lit(0.70710678, 100), lit(0.70710678, 50), 4);
                 c.px(img, 520, 200, lit(0.0, 200), lit(0.0, 100), lit(0.0, 50), 4);
               }});

  // ---- alpha test + culling --------------------------------------------------------------------------------------
  s.push_back({"alpha_test", "GX_GREATER 127: a left-to-right alpha ramp is cut in the middle, no blending",
               [] {
                 base_state();
                 GXSetAlphaCompare(GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0);
                 GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                 const f32 pts[4][3] = {{100, 100, 0}, {500, 100, 255}, {500, 300, 255}, {100, 300, 0}};
                 for (const auto& p : pts) {
                   GXPosition3f32(p[0], p[1], -1.f);
                   GXColor4u8(255, 255, 255, static_cast<u8>(p[2]));
                 }
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 160, 200, kGray, kGray, kGray);  // alpha ~38: discarded
                 c.px(img, 290, 200, kGray, kGray, kGray);  // alpha ~121: discarded
                 c.px(img, 330, 200, 255, 255, 255);        // alpha ~147: kept, opaque (no blend)
                 c.px(img, 460, 200, 255, 255, 255);
               }});

  const auto cull_scene = [](const char* name, const char* what, GXCullMode mode, bool cwVisible, bool ccwVisible) {
    return Scene{name, what,
                 [mode] {
                   base_state();
                   GXSetCullMode(mode);
                   // Visually counter-clockwise (bottom-left, bottom-right, apex) in green at x 100..300.
                   tri({100, 300, -1, 0, 255, 0, 255}, {300, 300, -1, 0, 255, 0, 255}, {200, 100, -1, 0, 255, 0, 255});
                   // Visually clockwise (same vertices, reversed order) in red at x 400..600.
                   tri({400, 300, -1, 255, 0, 0, 255}, {500, 100, -1, 255, 0, 0, 255}, {600, 300, -1, 255, 0, 0, 255});
                 },
                 [cwVisible, ccwVisible](const Image& img, Checker& c) {
                   if (ccwVisible) c.px(img, 200, 250, 0, 255, 0); else c.px(img, 200, 250, kGray, kGray, kGray);
                   if (cwVisible) c.px(img, 500, 250, 255, 0, 0); else c.px(img, 500, 250, kGray, kGray, kGray);
                 }};
  };
  // GX front faces are visually clockwise: upstream Aurora builds its pipelines with wgpu::FrontFace::CW.
  s.push_back(cull_scene("cull_none", "no culling: both windings drawn", GX_CULL_NONE, true, true));
  s.push_back(cull_scene("cull_back", "GX_CULL_BACK: the clockwise (front) triangle survives, the counter-clockwise one is culled",
                         GX_CULL_BACK, true, false));
  s.push_back(cull_scene("cull_front", "GX_CULL_FRONT: the counter-clockwise (back) triangle survives", GX_CULL_FRONT, false, true));

  // ---- indexed arrays through a display list (how J3D draws) -----------------------------------------------------
  s.push_back({"display_list_indexed", "GX_INDEX16 position/colour arrays (big-endian, as in game memory) via a recorded display list",
               [] {
                 static uint32_t pos[4][3];
                 static uint8_t clr[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
                 const f32 pts[4][2] = {{100, 100}, {500, 100}, {500, 400}, {100, 400}};
                 for (int i = 0; i < 4; ++i) {
                   pos[i][0] = be32(pts[i][0]);
                   pos[i][1] = be32(pts[i][1]);
                   pos[i][2] = be32(-1.f);
                 }
                 base_state();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
                 GXSetVtxDesc(GX_VA_CLR0, GX_INDEX16);
                 GXSETARRAY(GX_VA_POS, pos, sizeof(pos), 12, false);
                 GXSETARRAY(GX_VA_CLR0, clr, sizeof(clr), 4, false);
                 alignas(32) static uint8_t dl[1024];
                 GXBeginDisplayList(dl, sizeof(dl));
                 GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
                 for (const u16 idx : {0, 1, 2, 0, 2, 3}) {
                   GXPosition1x16(idx);
                   GXColor1x16(idx);
                 }
                 GXEnd();
                 const u32 size = GXEndDisplayList();
                 GXCallDisplayList(dl, size);
               },
               [](const Image& img, Checker& c) {
                 // Four-corner bilinear-ish gradient from two triangles: near each corner the vertex colour dominates.
                 c.px(img, 108, 108, 247, 8, 0, 12);
                 c.px(img, 492, 108, 8, 247, 0, 12);
                 c.px(img, 108, 392, 247, 247, 0, 12);
                 c.px(img, 492, 392, 8, 0, 247, 14);
                 c.px(img, 50, 50, kGray, kGray, kGray);
               }});

  // ---- lines and points (expanded into screen-space quads by the vertex shader) ----------------------------------
  s.push_back({"line_width", "GX_LINES, width 24/6 = 4 px: thickness (4 rows) and end points",
               [] {
                 base_state();
                 GXSetLineWidth(24, GX_TO_ZERO);
                 GXBegin(GX_LINES, GX_VTXFMT0, 2);
                 GXPosition3f32(100, 200, -1.f);
                 GXColor4u8(0, 255, 0, 255);
                 GXPosition3f32(500, 200, -1.f);
                 GXColor4u8(0, 255, 0, 255);
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 for (const int y : {198, 199, 200, 201}) c.px(img, 300, y, 0, 255, 0);
                 c.px(img, 300, 197, kGray, kGray, kGray);
                 c.px(img, 300, 202, kGray, kGray, kGray);
                 c.px(img, 105, 200, 0, 255, 0);
                 c.px(img, 495, 200, 0, 255, 0);
                 c.px(img, 95, 200, kGray, kGray, kGray);
                 c.px(img, 505, 200, kGray, kGray, kGray);
               }});

  s.push_back({"point_size", "GX_POINTS, size 48/6 = 8 px: an 8x8 square centred on the vertex",
               [] {
                 base_state();
                 GXSetPointSize(48, GX_TO_ZERO);
                 GXBegin(GX_POINTS, GX_VTXFMT0, 1);
                 GXPosition3f32(300, 300, -1.f);
                 GXColor4u8(255, 0, 255, 255);
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 296, 296, 255, 0, 255);
                 c.px(img, 303, 303, 255, 0, 255);
                 c.px(img, 300, 300, 255, 0, 255);
                 c.px(img, 295, 300, kGray, kGray, kGray);
                 c.px(img, 304, 300, kGray, kGray, kGray);
                 c.px(img, 300, 295, kGray, kGray, kGray);
                 c.px(img, 300, 304, kGray, kGray, kGray);
               }});

  // Line texture offset: the bit set of offset texcoords is a u32 in the uniform blob; read as a float it is a
  // denormal and the offset never applied (the same defect that disabled all lights). With the offset on, the two
  // long edges of the line sample different texture rows; with it off, both edges sample the same row.
  const auto offset_line = [](GXBool lineOffsetOn) {
    return [lineOffsetOn] {
      build_test_texture();
      textured_state(gTexRgba8, 4, 4, GX_TF_RGBA8, GX_REPLACE);
      GXSetLineWidth(24, GX_TO_HALF);
      GXEnableTexOffsets(GX_TEXCOORD0, lineOffsetOn, GX_DISABLE);
      GXBegin(GX_LINES, GX_VTXFMT0, 2);
      GXPosition3f32(100, 200, -1.f);
      GXColor4u8(255, 255, 255, 255);
      GXTexCoord2f32(0.f, 0.125f);
      GXPosition3f32(500, 200, -1.f);
      GXColor4u8(255, 255, 255, 255);
      GXTexCoord2f32(1.f, 0.125f);
      GXEnd();
    };
  };
  s.push_back({"line_tex_offset_off", "control: texcoord offsets disabled, both long edges of the line sample row 0",
               offset_line(GX_DISABLE),
               [](const Image& img, Checker& c) {
                 // x = 180 -> s = 0.2 -> texel column 0; row 0 = (40,40,30)
                 c.px(img, 180, 198, 40, 40, 30, 2);
                 c.px(img, 180, 201, 40, 40, 30, 2);
               }});
  s.push_back({"line_tex_offset_on", "texcoord offset +0.5 enabled: one long edge samples row 0, the other row 2 (which one is not pinned)",
               offset_line(GX_ENABLE),
               [](const Image& img, Checker& c) {
                 const auto a = img.at(180, 198), b = img.at(180, 201);
                 const auto near = [](const std::array<int, 4>& p, int g) {
                   return std::abs(p[0] - 40) <= 2 && std::abs(p[1] - g) <= 2 && std::abs(p[2] - 30) <= 2;
                 };
                 // row 0 = (40,40,30), row 2 = (40,160,30)
                 const bool ok = (near(a, 40) && near(b, 160)) || (near(a, 160) && near(b, 40));
                 char buf[160];
                 std::snprintf(buf, sizeof(buf), "edge pixels (180,198)=(%d,%d,%d) (180,201)=(%d,%d,%d); want {row0,row2} in either order",
                               a[0], a[1], a[2], b[0], b[1], b[2]);
                 c.is(ok, buf);
               }});

  // ---- scissor, viewport, EFB copy, fog ---------------------------------------------------------------------------
  s.push_back({"scissor", "GXSetScissor(100,100,200,100) clips a full-screen rect to exactly that pixel rectangle",
               [] {
                 base_state();
                 GXSetScissor(100, 100, 200, 100);
                 rect(0, 0, W, H, 1, 0, 255, 0);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 100, 100, 0, 255, 0);
                 c.px(img, 299, 199, 0, 255, 0);
                 c.px(img, 200, 150, 0, 255, 0);
                 for (const auto& [x, y] : {std::pair{99, 150}, {300, 150}, {200, 99}, {200, 200}, {10, 10}}) {
                   c.px(img, x, y, kGray, kGray, kGray);
                 }
               }});

  s.push_back({"viewport", "GXSetViewport(320,240,320,240): a full-ortho rect lands exactly in the lower-right quarter",
               [] {
                 base_state();
                 GXSetViewport(320.f, 240.f, 320.f, 240.f, 0.f, 1.f);
                 rect(0, 0, W, H, 1, 255, 128, 0);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 320, 240, 255, 128, 0);
                 c.px(img, 639, 479, 255, 128, 0);
                 c.px(img, 480, 360, 255, 128, 0);
                 for (const auto& [x, y] : {std::pair{319, 300}, {400, 239}, {100, 100}, {319, 239}}) {
                   c.px(img, x, y, kGray, kGray, kGray);
                 }
               }});

  s.push_back({"efb_copy_reuse",
               "copy a 128x128 EFB corner (4 colour quadrants) to a texture, wipe the EFB, redraw the copy elsewhere: "
               "colours, row order, and no leftover",
               [] {
                 static uint8_t copyBuf[128 * 128 * 4]; // only the address is used: copies live on the GPU
                 base_state();
                 rect(0, 0, 64, 64, 1, 255, 0, 0);      // top-left red
                 rect(64, 0, 128, 64, 1, 0, 255, 0);    // top-right green
                 rect(0, 64, 64, 128, 1, 0, 0, 255);    // bottom-left blue
                 rect(64, 64, 128, 128, 1, 255, 255, 0); // bottom-right yellow
                 GXSetTexCopySrc(0, 0, 128, 128);
                 GXSetTexCopyDst(128, 128, GX_TF_RGBA8, GX_FALSE);
                 GXCopyTex(copyBuf, GX_FALSE);
                 rect(0, 0, W, H, 2, 20, 20, 20); // wipe everything the copy came from
                 textured_state(copyBuf, 128, 128, GX_TF_RGBA8, GX_REPLACE);
                 tex_rect(300, 200, 428, 328, 1);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 300 + 32, 200 + 32, 255, 0, 0, 4);
                 c.px(img, 300 + 96, 200 + 32, 0, 255, 0, 4);
                 c.px(img, 300 + 32, 200 + 96, 0, 0, 255, 4);
                 c.px(img, 300 + 96, 200 + 96, 255, 255, 0, 4);
                 c.px(img, 32, 32, 20, 20, 20);   // the source region really was wiped
                 c.px(img, 200, 300, 20, 20, 20); // and nothing leaked elsewhere
               }});

  // GX_FOG_ORTHO_LIN: factor = (distance - start) / (end - start), clamped; colour = lerp(colour, fog, factor).
  s.push_back({"fog_ortho_linear", "orthographic linear fog 20..80, fog colour blue: factors 0, 0.5 and 1 at distance 20, 50, 80",
               [] {
                 base_state();
                 GXSetFog(GX_FOG_ORTHO_LIN, 20.f, 80.f, 0.1f, 100.f, GXColor{0, 0, 255, 255});
                 rect(40, 100, 180, 300, 20, 200, 100, 0);
                 rect(240, 100, 380, 300, 50, 200, 100, 0);
                 rect(440, 100, 580, 300, 80, 200, 100, 0);
               },
               [](const Image& img, Checker& c) {
                 const auto fogged = [&](int x, double f) {
                   c.px(img, x, 200, static_cast<int>(200 * (1 - f) + 0.5), static_cast<int>(100 * (1 - f) + 0.5),
                        static_cast<int>(255 * f + 0.5), 5);
                 };
                 fogged(110, 0.0);
                 fogged(310, 0.5);
                 fogged(510, 1.0);
               }});

  // Fog range adjustment with the game's own table (d_kankyo_data.cpp S_xfog_table_data[0], centre 0x140, enabled at
  // d_kankyo.cpp:9459). Upstream (gx/shader.cpp, command_processor.cpp build_fog_range_lut) computes
  //   fogF = clamp(fogBase * lut[x] - c),  lut[x] = sqrt(o^2 + k^2) / k,  o = pixel-centre NDC x - centre,
  //   k = linear interpolation of rangeK[i] / 64 at index 9 - 9|o|, rangeK pairs swapped.
  // The reference below transcribes that, so this checks the Vita path (LUT encode, upload, sampling at
  // gl_FragCoord.x, the multiply), not upstream's own maths.
  const auto fog_range_expected = [](int x, double base, double c) {
    static const unsigned short r[10] = {260, 260, 270, 280, 290, 300, 310, 320, 330, 340};
    double rangeK[10];
    for (unsigned i = 0; i < 10; ++i) rangeK[i] = r[(i & ~1u) | (1u - (i & 1u))] / 64.0;
    const double screenX = ((x + 0.5) / 640.0) * 2.0 - 1.0;
    const double offset = screenX - 0.0; // centre 320 of 640 -> NDC 0
    const double index = std::clamp(9.0 - std::abs(offset) * 9.0, 0.0, 9.0);
    const unsigned lower = static_cast<unsigned>(index);
    const unsigned upper = std::min(lower + 1, 9u);
    const double fraction = index - lower;
    const double k = std::max(rangeK[lower] * (1.0 - fraction) + rangeK[upper] * fraction, 1e-6);
    const double lut = std::sqrt(offset * offset + k * k) / k;
    return std::clamp(base * lut - c, 0.0, 1.0);
  };
  const auto fog_range_scene = [](bool rangeOn) {
    return [rangeOn] {
      static GXFogAdjTable table = {{260, 260, 270, 280, 290, 300, 310, 320, 330, 340}};
      base_state();
      GXSetFog(GX_FOG_ORTHO_LIN, 40.f, 60.f, 0.1f, 100.f, GXColor{0, 0, 255, 255});
      GXSetFogRangeAdj(rangeOn ? GX_ENABLE : GX_DISABLE, 320, rangeOn ? &table : nullptr);
      rect(0, 100, W, 300, 50, 200, 100, 0);
    };
  };
  // fogBase = a * depth, a = (far-near)/(end-start), depth = (d-near)/(far-near)  =>  (d-near)/(end-start)
  const double fogBase = (50.0 - 0.1) / (60.0 - 40.0);
  const double fogC = (40.0 - 0.1) / (60.0 - 40.0);
  s.push_back({"fog_range_off", "control: ortho linear fog 40..60 at distance 50, range adjust off: every column has factor 0.5",
               fog_range_scene(false),
               [fogBase, fogC](const Image& img, Checker& c) {
                 const double f = std::clamp(fogBase - fogC, 0.0, 1.0);
                 for (const int x : {2, 160, 320, 480, 637}) {
                   c.px(img, x, 200, static_cast<int>(200 * (1 - f) + 0.5), static_cast<int>(100 * (1 - f) + 0.5),
                        static_cast<int>(255 * f + 0.5), 5);
                 }
               }});
  s.push_back({"fog_range_on", "game's fog-range table: the factor grows toward the screen edges exactly as the upstream LUT says",
               fog_range_scene(true),
               [fogBase, fogC, fog_range_expected](const Image& img, Checker& c) {
                 for (const int x : {2, 160, 320, 480, 637}) {
                   const double f = fog_range_expected(x, fogBase, fogC);
                   c.px(img, x, 200, static_cast<int>(200 * (1 - f) + 0.5), static_cast<int>(100 * (1 - f) + 0.5),
                        static_cast<int>(255 * f + 0.5), 6);
                 }
                 // The centre/edge difference must be visible, else the scene proves nothing (expected ~19 levels).
                 c.is(std::abs(img.at(2, 200)[2] - img.at(320, 200)[2]) >= 10, "edge and centre have the same fog factor");
               }});

  // ---- perspective fog: the only fog type Twilight Princess uses (GX_FOG_PERSP_LIN, d_kankyo.cpp:9396/9431/9453) ----
  // GX computes fog per pixel from the 24-bit depth: factor = clamp(A / (B - z) - C) with z = NDC z + 1 in [0, 1] and
  //   A = f*n / ((f-n)(end-start)),  B = f / (f-n),  C = start / (end-start).
  // For a point at eye distance d, z = B - f*n / ((f-n) d), so A / (B - z) - C = (d - start) / (end - start): the
  // reference below is that distance formula, not the A/B/C arithmetic, so it also checks the projection <-> fog pairing.
  struct PerspFog {
    double n, f, start, end;
    std::vector<double> depths; // one fronto-parallel quad per depth, left to right
  };
  // Fog set with GXSetFog (d_kankyo.cpp:9396, 9431), or with GFSetFog recorded into a display list (d_kankyo.cpp:9453),
  // which writes the BP registers itself. GFSetFog is the game's own code (libs/dolphin/src/gf/GFPixel.cpp).
  const auto persp_quads = [](const PerspFog& p, bool viaDisplayList) {
    return [p, viaDisplayList] {
      base_state();
      set_perspective(p.n, p.f);
      if (viaDisplayList) {
        alignas(32) static uint8_t dl[256];
        GXBeginDisplayList(dl, sizeof(dl));
        GFSetFog(GX_FOG_PERSP_LIN, static_cast<f32>(p.start), static_cast<f32>(p.end), static_cast<f32>(p.n),
                 static_cast<f32>(p.f), kFogBlue);
        const u32 size = GXEndDisplayList();
        GXCallDisplayList(dl, size);
      } else {
        GXSetFog(GX_FOG_PERSP_LIN, static_cast<f32>(p.start), static_cast<f32>(p.end), static_cast<f32>(p.n),
                 static_cast<f32>(p.f), kFogBlue);
      }
      for (size_t i = 0; i < p.depths.size(); ++i) {
        const double x0 = 20.0 + 120.0 * static_cast<double>(i);
        persp_rect(x0, 100, x0 + 100, 300, p.depths[i], 200, 100, 0);
      }
    };
  };
  const auto persp_quads_check = [](const PerspFog& p, int tol) {
    return [p, tol](const Image& img, Checker& c) {
      for (size_t i = 0; i < p.depths.size(); ++i) {
        const double f = linear_fog(p.depths[i], p.start, p.end);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(quad at distance %g, factor %.3f)", p.depths[i], f);
        if (!check_fogged(img, c, 20 + 120 * static_cast<int>(i) + 50, 200, f, tol)) {
          std::printf("    %s\n", buf);
        }
      }
    };
  };
  // Distances 50 and 100 are before and at the fog start (factor 0), 300 is halfway, 500 is the end, 800 is past it.
  const PerspFog kNear{10, 1000, 100, 500, {50, 100, 300, 500, 800}};
  s.push_back({"fog_persp_linear", "GX_FOG_PERSP_LIN 100..500 (near 10, far 1000): factors 0, 0, 0.5, 1, 1 at distance 50, 100, 300, 500, 800",
               persp_quads(kNear, false), persp_quads_check(kNear, 4)});
  s.push_back({"fog_persp_gf_displaylist", "the same fog written by the game's GFSetFog into a display list (how d_kankyo.cpp:9453 does it)",
               persp_quads(kNear, true), persp_quads_check(kNear, 4)});

  // The planes TP falls back to when no view is set (d_kankyo.cpp:9382-9383, 9414-9415, 9436-9437: near 1, far 160000)
  // with an illustrative fog range in world units (the real range comes from the stage's environment data). B - z is about 1/d here, so a float32 depth costs about half a level at 50000: tolerance 5.
  const PerspFog kFar{1, 160000, 5000, 50000, {2000, 5000, 27500, 50000, 120000}};
  s.push_back({"fog_persp_far", "GX_FOG_PERSP_LIN 5000..50000 with near 1, far 160000 (TP's fallback planes): factors 0, 0, 0.5, 1, 1",
               persp_quads(kFar, false), persp_quads_check(kFar, 5)});

  // Fog must follow the depth of each pixel, not of each vertex: a ground plane 10 below the eye runs from distance 20 to
  // 900. At screen row r (pixel centre r + 0.5) the ground point projects to NDC y = cot * (-h) / d, so
  //   d = h * cot / (2 (r + 0.5) / H - 1)
  // (1/d is linear in screen y, d is not: a per-vertex distance would be far off in the middle of the plane).
  constexpr double kGroundH = 10.0, kGroundStart = 20.0, kGroundEnd = 200.0;
  s.push_back({"fog_persp_ground", "perspective fog on a ground plane receding to the horizon: the factor follows the per-pixel distance",
               [] {
                 base_state();
                 set_perspective(10, 1000);
                 GXSetFog(GX_FOG_PERSP_LIN, kGroundStart, kGroundEnd, 10.f, 1000.f, kFogBlue);
                 GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                 for (const auto& [x, z] : {std::pair{-400.f, -20.f}, {400.f, -20.f}, {400.f, -900.f}, {-400.f, -900.f}}) {
                   GXPosition3f32(x, static_cast<f32>(-kGroundH), z);
                   GXColor4u8(200, 100, 0, 255);
                 }
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 for (const int row : {262, 270, 280, 300, 350, 440}) {
                   const double d = kGroundH * kCot / (2.0 * (row + 0.5) / H - 1.0);
                   const double f = linear_fog(d, kGroundStart, kGroundEnd);
                   if (!check_fogged(img, c, 320, row, f, 4)) {
                     std::printf("    (row %d is at distance %.1f, factor %.3f)\n", row, d, f);
                   }
                 }
                 // The spread has to be wide, else the scene proves nothing about per-pixel interpolation.
                 c.is(img.at(320, 262)[2] - img.at(320, 350)[2] >= 150, "fog factor does not vary down the plane");
                 c.px(img, 320, 200, kGray, kGray, kGray); // above the horizon: nothing
               }});

  // GX also has exponential perspective fog types; TP never selects them (they are debug-UI combo items), so this is
  // a renderer-completeness check, not a game path. With x = (d - start) / (end - start) (Dolphin):
  //   EXP = 1 - 2^(-8x), EXP2 = 1 - 2^(-8 x^2), REVEXP = 2^(-8 (1-x)), REVEXP2 = 2^(-8 (1-x)^2).
  s.push_back({"fog_persp_types", "(not a TP path) perspective EXP / EXP2 / REVEXP / REVEXP2 at x = 0.25 and 0.5, fog 100..500",
               [] {
                 base_state();
                 set_perspective(10, 1000);
                 const GXFogType types[4] = {GX_FOG_PERSP_EXP, GX_FOG_PERSP_EXP2, GX_FOG_PERSP_REVEXP, GX_FOG_PERSP_REVEXP2};
                 for (int i = 0; i < 4; ++i) {
                   GXSetFog(types[i], 100.f, 500.f, 10.f, 1000.f, kFogBlue);
                   const double x0 = 20.0 + 155.0 * i;
                   persp_rect(x0, 60, x0 + 135, 200, 200, 200, 100, 0);  // x = 0.25
                   persp_rect(x0, 260, x0 + 135, 400, 300, 200, 100, 0); // x = 0.5
                 }
               },
               [](const Image& img, Checker& c) {
                 const auto factor = [](int type, double x) {
                   switch (type) {
                   case 0: return 1.0 - std::exp2(-8.0 * x);
                   case 1: return 1.0 - std::exp2(-8.0 * x * x);
                   case 2: return std::exp2(-8.0 * (1.0 - x));
                   default: return std::exp2(-8.0 * (1.0 - x) * (1.0 - x));
                   }
                 };
                 for (int type = 0; type < 4; ++type) {
                   const int cx = 20 + 155 * type + 67;
                   check_fogged(img, c, cx, 130, factor(type, 0.25), 5);
                   check_fogged(img, c, cx, 330, factor(type, 0.5), 5);
                 }
               }});

  // The replay builds a vertex program with only as many light slots as the draw's masks need (1, 2, 4, 6 or 8; see
  // gx::vita::light_bucket). Four quads, each with a different mask, hit four different programs. Lights outside a
  // quad's mask are loaded with non-zero colours and must not contribute, and lights above the bucket must not be lost.
  s.push_back({"lighting_masks", "per-draw light masks (0+5, 1, 3, 6): only the enabled lights light each quad, whatever slot count the program has",
               [] {
                 base_state();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
                 static GXLightObj lights[8];
                 for (int i = 0; i < 8; ++i) {
                   GXInitLightColor(&lights[i], GXColor{kLightColors[i][0], kLightColors[i][1], kLightColors[i][2], 255});
                   GXInitLightPos(&lights[i], 0.f, 0.f, 1048576.f); // infinite light travelling along -z
                   GXLoadLightObjImm(&lights[i], static_cast<GXLightID>(1u << i));
                 }
                 GXSetChanAmbColor(GX_COLOR0A0, GXColor{30, 30, 30, 255});
                 GXSetChanMatColor(GX_COLOR0A0, GXColor{200, 200, 200, 255});
                 for (int q = 0; q < 4; ++q) {
                   GXSetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, kMasks[q], GX_DF_CLAMP, GX_AF_NONE);
                   const f32 x0 = 20.f + q * 150.f;
                   GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                   for (const auto& [x, y] : {std::pair{x0, 100.f}, {x0 + 120, 100.f}, {x0 + 120, 300.f}, {x0, 300.f}}) {
                     GXPosition3f32(x, y, -1.f);
                     GXNormal3f32(0.f, 0.f, 1.f);
                   }
                   GXEnd();
                 }
               },
               [](const Image& img, Checker& c) {
                 for (int q = 0; q < 4; ++q) {
                   int sum[3] = {30, 30, 30};
                   for (int i = 0; i < 8; ++i) {
                     if (kMasks[q] & (1u << i)) {
                       for (int k = 0; k < 3; ++k) sum[k] += kLightColors[i][k];
                     }
                   }
                   const auto lit = [](int v) { return static_cast<int>(200.0 * std::min(255, v) / 255.0 + 0.5); };
                   c.px(img, 20 + q * 150 + 60, 200, lit(sum[0]), lit(sum[1]), lit(sum[2]), 4);
                 }
               }});

  add_texture_scenes(s);
  add_texobj_scenes(s);
  add_indirect_scenes(s);
  add_tp_line_scenes(s);
  add_alpha_scenes(s);
  add_offscreen_scenes(s);
  add_present_scenes(s);
  return s;
}
} // namespace

// A TextureRef owned by a static lives until static teardown: after main() and possibly after the texture module's own
// statics are gone. Its destructor queues a GL deletion, which must not touch freed state.
aurora::gfx::TextureHandle gStaticTextureHolder;

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!hostgl::init(kSurfaceW, kSurfaceH)) {
    std::printf("no GLES2 context\n");
    return 2;
  }
  aurora::gfx::vita::set_present_size(W, H);
  aurora::gx::fifo::init();
  GXInit(nullptr, 0);

  // The clear colour is captured at begin_frame (as in upstream Aurora), before this frame's FIFO commands are
  // drained, so GXInit's GXSetCopyClear only takes effect from the second frame on. Settle that first.
  render_frame([] { base_state(); });

  const std::vector<Scene> scenes = make_scenes();
  int total = 0, failedScenes = 0, ran = 0;
  for (const Scene& scene : scenes) {
    if (argc > 1) {
      bool wanted = false;
      for (int i = 1; i < argc; ++i) {
        wanted = wanted || std::strcmp(argv[i], scene.name) == 0;
      }
      if (!wanted) {
        continue;
      }
    }
    ++ran;
    Checker checker;
    checker.scene = scene.name;
    if (scene.warmup) {
      render_frame(scene.warmup);
    }
    const Image img = render_frame(scene.draw, scene.presentW != 0 ? scene.presentW : W,
                                   scene.presentH != 0 ? scene.presentH : H);
    checker.audit = &gAudit;
    save_png(scene.name, img);
    scene.check(img, checker);
    if (checker.failures > Checker::kMaxPrinted) {
      std::printf("  ... and %d more failing checks in [%s]\n", checker.failures - Checker::kMaxPrinted, scene.name);
    }
    total += checker.checks;
    failedScenes += checker.failures != 0;
    std::printf("%-24s %s  (%d checks)  %s\n", scene.name, checker.failures == 0 ? "ok  " : "FAIL", checker.checks,
                scene.what);
  }
  // The backend's shutdown path (aurora::shutdown() in vita/aurora/backend.cpp) is fifo::shutdown() then shutdown_gl():
  // together they must release every texture the GX layer cached, while the GL context is still current.
  aurora::gx::fifo::shutdown();
  aurora::gfx::vita::shutdown_gl();
  const uint64_t leakedBytes = aurora::gfx::live_texture_bytes();
  ++total;
  failedScenes += leakedBytes != 0;
  if (leakedBytes != 0) {
    std::printf("FAIL [teardown] %llu bytes of texture are still live after shutdown_gl()\n",
                static_cast<unsigned long long>(leakedBytes));
  }
  std::printf("%-24s %s  (1 checks)  shutdown_gl() releases every cached texture (live texture bytes == 0)\n", "teardown",
              leakedBytes == 0 ? "ok  " : "FAIL");
  std::printf("%d scenes, %d pixel checks, %d scenes failed\n", ran, total, failedScenes);
  {
    // Static-teardown check (see gStaticTextureHolder): leave one queued deletion behind, so the list owns storage, and
    // one texture held by a static. A crash or sanitizer report when the process exits is the failure.
    const uint32_t pixel = 0xff00ff00u;
    aurora::gfx::new_texture_rgba8(1, 1, 1, &pixel, GX_TF_RGBA8_PC, "static teardown probe");
    gStaticTextureHolder = aurora::gfx::new_texture_rgba8(1, 1, 1, &pixel, GX_TF_RGBA8_PC, "static teardown holder");
  }
  hostgl::shutdown();
  return failedScenes != 0 || ran == 0;
}
