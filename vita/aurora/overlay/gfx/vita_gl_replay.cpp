// Vita backend: GL replay of a recorded frame.
//
// Everything that touches OpenGL ES 2 (vitaGL on Vita, Mesa in tests/host) is here, plus the GX
// state -> GL state translation that upstream does in gx/gx.cpp (blend/depth/cull/polygon offset).
//
// Conventions (decided once, shared with the shader generator):
//  - The EFB is an RGBA8 texture + D24S8 renderbuffer in an FBO. The vertex shader negates clip Y,
//    so FBO row 0 (GL window y = 0) is the *top* of the GX image. GX viewport/scissor rectangles
//    (origin top-left) therefore map to glViewport/glScissor unchanged, and EFB copies sample the
//    EFB texture with v = y / height. glFrontFace(GL_CCW) then equals WebGPU's FrontFace::CW.
//  - Depth is GL [-w, w] (the shader maps upstream's [0, w]); glDepthRangef carries GX znear/zfar.
//  - vitaGL has no blend constant, so GX destination alpha is written by the fragment shader
//    (ProgramSource::usesExtra, uExtra.x) instead of via the blend constant.
#include "vita_frame.hpp"

// The thread census / affinity module is Vita-only: this file is also compiled for the host e2e harness,
// where there is no psp2/ and no motherboard to place threads on. Guarding the include AND the one call
// site keeps both builds working from one source.
#ifdef __vita__
#include "cpu_placement.hpp"

#include <psp2/kernel/threadmgr.h>
#endif

#include "vita_gl.hpp"
#include "vita_shader_dump.hpp"
#include "vita_texture.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../gx/gx.hpp"
#include "../gx/shader_info.hpp"
#include "../gx/vita_gx.hpp"
#include "../logging.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>


namespace aurora::gfx::vita {
namespace {
Module Log("aurora::gfx::vita");

// FBO row order (UNVERIFIED on hardware). The code assumes GL-standard FBOs, as on desktop GL/Mesa (no host test
// exercises the replay yet): the vertex shader negates clip Y, so FBO row 0 holds the GX top row, and present/EFB copies
// sample it accordingly. vitaGL's opt-in HAVE_UNFLIPPED_FBOS build flag (e.g. DaedalusX64) stores FBO rows
// upside down instead. We do not know which flags the vdpm vitaGL package was built with; the references
// only say the flag is opt-in. If the first on-device frame is vertically flipped, set this to true (it
// flips the present pass and the two EFB sampling sites).
constexpr bool kFboRowsFlipped = false;
// vitaGL's glDrawArrays reads from a built-in index buffer of this many entries.
constexpr GLsizei MaxArrayVertices = 49152;
constexpr size_t MaxOffscreenTargets = 8;

#ifdef GL_DEPTH24_STENCIL8
constexpr GLenum DepthStencilFormat = GL_DEPTH24_STENCIL8;
#else
constexpr GLenum DepthStencilFormat = GL_DEPTH24_STENCIL8_OES;
#endif

// ---- GL helpers -------------------------------------------------------------------------------

// Where a refused shader's source is saved (set_shader_dump_dir; empty = never), and how many were saved so far:
// a systematic failure would otherwise write one pair of files per program.
std::string sShaderDumpDir;
int sShaderDumps = 0;
constexpr int MaxShaderDumps = 8;

GLuint compile_shader(GLenum type, const std::string& source, const char* what) {
  const GLuint shader = glCreateShader(type);
  const char* src = source.c_str();
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    Log.error("{} shader compile failed: {}", what, log);
    if (!sShaderDumpDir.empty() && sShaderDumps < MaxShaderDumps) {
      ++sShaderDumps;
      const std::string path =
          dump_shader_source(sShaderDumpDir, what, type == GL_VERTEX_SHADER ? "vert" : "frag", source);
      Log.error("{} shader source {}", what, path.empty() ? "could not be saved" : "saved to " + path);
    }
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// Links a program; `attribs` are bound to locations 0..n-1 in order.
GLuint link_program(const std::string& vs, const std::string& fs, const char* const* attribs, size_t attribCount,
                    const char* what) {
  const GLuint v = compile_shader(GL_VERTEX_SHADER, vs, what);
  const GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, what);
  if (v == 0 || f == 0) {
    if (v != 0) {
      glDeleteShader(v);
    }
    if (f != 0) {
      glDeleteShader(f);
    }
    return 0;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, v);
  glAttachShader(program, f);
  for (size_t i = 0; i < attribCount; ++i) {
    glBindAttribLocation(program, static_cast<GLuint>(i), attribs[i]);
  }
  glLinkProgram(program);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  // The shader objects are only needed until the program is linked.
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok) {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
    Log.error("{} program link failed: {}", what, log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

// ---- cached GL state --------------------------------------------------------------------------

constexpr int8_t Unknown = -1;
struct BlendDesc {
  bool enable = false;
  GLenum equation = GL_FUNC_ADD;
  GLenum srcRgb = GL_ONE;
  GLenum dstRgb = GL_ZERO;
  GLenum srcAlpha = GL_ONE;
  GLenum dstAlpha = GL_ZERO;
  bool operator==(const BlendDesc&) const = default;
};
struct PassState {
  GLenum depthFunc = GL_ALWAYS;
  bool depthMask = false;
  GLenum cullFace = 0; // 0 = culling off
  BlendDesc blend;
  bool colorMask[4] = {true, true, true, true};
  float polyScale = 0.f;
  float polyBias = 0.f;
};

// Texture units the replay binds: the GX texture maps on units 0..GX_MAX_TEXMAP-1, and the fog-range table on unit 15
// (see Program::usesFogRange). The binding cache below covers all of them; an array sized by GX_MAX_TEXMAP alone was
// written out of bounds at index 15 on every frame that used fog range adjustment.
constexpr GLuint kFogRangeUnit = 15;
constexpr uint32_t kTextureUnits = kFogRangeUnit + 1;
static_assert(kTextureUnits >= GX_MAX_TEXMAP, "the GX texture maps must fit below the fog-range unit");

struct GlCache {
  int8_t depthTest = Unknown;
  int8_t cullEnabled = Unknown;
  int8_t blendEnabled = Unknown;
  int8_t polyOffset = Unknown;
  int8_t scissorTest = Unknown;
  PassState pass;
  bool passValid = false;
  GLuint program = 0xFFFFFFFF;
  GLuint boundTexture[kTextureUnits]{};
  bool boundTextureValid[kTextureUnits]{};
  uint32_t enabledAttribs = 0;
  GLuint activeUnit = 0xFFFFFFFF;
};
GlCache sCache;

// Counters for the `pass` split (HANDOVER 7.25). A cache hit is NOT work, and the whole point of asking
// where `pass` goes is to find out how much of it is already suppressed by the caches above. So these
// count calls that actually reached gl*, not calls that were made.
uint64_t sTextureBindCount = 0; // glBindTexture issued by bind_texture()
uint64_t sFogUploadCount = 0;   // glTexImage2D issued by bind_fog_range()

void invalidate_cache() {
  sCache = {};
  // Attribute arrays are left as they are; enabledAttribs is reset because we disable them below.
  for (uint32_t i = 0; i < 16; ++i) {
    glDisableVertexAttribArray(i);
  }
}

void set_enabled(GLenum cap, int8_t& shadow, bool enable) {
  if (shadow != static_cast<int8_t>(enable)) {
    shadow = static_cast<int8_t>(enable);
    if (enable) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }
}

void use_program(GLuint program) {
  if (sCache.program != program) {
    sCache.program = program;
    glUseProgram(program);
  }
}

void bind_texture(uint32_t unit, GLuint texture) {
  if (sCache.activeUnit != unit) {
    sCache.activeUnit = unit;
    glActiveTexture(GL_TEXTURE0 + unit);
  }
  if (!sCache.boundTextureValid[unit] || sCache.boundTexture[unit] != texture) {
    sCache.boundTexture[unit] = texture;
    sCache.boundTextureValid[unit] = true;
    ++sTextureBindCount;
    glBindTexture(GL_TEXTURE_2D, texture);
  }
}

void enable_attribs(uint32_t mask) {
  const uint32_t changed = mask ^ sCache.enabledAttribs;
  for (uint32_t i = 0; changed >> i != 0; ++i) {
    if ((changed >> i) & 1u) {
      if ((mask >> i) & 1u) {
        glEnableVertexAttribArray(i);
      } else {
        glDisableVertexAttribArray(i);
      }
    }
  }
  sCache.enabledAttribs = mask;
}

// ---- GX state -> GL state ---------------------------------------------------------------------

GLenum to_compare(GXCompare func) {
  switch (func) {
  case GX_NEVER:
    return GL_NEVER;
  case GX_LESS:
    return GL_LESS;
  case GX_EQUAL:
    return GL_EQUAL;
  case GX_LEQUAL:
    return GL_LEQUAL;
  case GX_GREATER:
    return GL_GREATER;
  case GX_NEQUAL:
    return GL_NOTEQUAL;
  case GX_GEQUAL:
    return GL_GEQUAL;
  case GX_ALWAYS:
  default:
    return GL_ALWAYS;
  }
}

// Same mapping as upstream to_blend_factor(): a "source color" factor on the source side means the
// destination color (and vice versa), because GX only has one color-factor enum for both sides.
GLenum to_blend_factor(GXBlendFactor factor, bool isDst) {
  switch (factor) {
  case GX_BL_ZERO:
    return GL_ZERO;
  case GX_BL_ONE:
    return GL_ONE;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    return isDst ? GL_SRC_COLOR : GL_DST_COLOR;
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    return isDst ? GL_ONE_MINUS_SRC_COLOR : GL_ONE_MINUS_DST_COLOR;
  case GX_BL_SRCALPHA:
    return GL_SRC_ALPHA;
  case GX_BL_INVSRCALPHA:
    return GL_ONE_MINUS_SRC_ALPHA;
  case GX_BL_DSTALPHA:
    return GL_DST_ALPHA;
  case GX_BL_INVDSTALPHA:
    return GL_ONE_MINUS_DST_ALPHA;
  default:
    return GL_ONE;
  }
}

bool uses_source_alpha(GXBlendFactor factor) { return factor == GX_BL_SRCALPHA || factor == GX_BL_INVSRCALPHA; }

// Mirrors upstream gx/pipeline.cpp create_pipeline().
bool dst_alpha_active(const gx::PipelineConfig& c) {
  return c.alphaUpdate && c.dstAlpha != UINT32_MAX && c.dstAlpha != 0;
}
bool needs_source_alpha(const gx::PipelineConfig& c) {
  return c.colorUpdate && c.blendMode == GX_BM_BLEND &&
         (uses_source_alpha(c.blendFacSrc) || uses_source_alpha(c.blendFacDst));
}
// The fragment shader writes the constant destination alpha itself (see ProgramSource::usesExtra).
bool replaces_alpha(const gx::PipelineConfig& c) { return dst_alpha_active(c) && !needs_source_alpha(c); }

BlendDesc to_blend_desc(const gx::PipelineConfig& c, bool replace) {
  BlendDesc d;
  switch (c.blendMode) {
  case GX_BM_BLEND:
    d.srcRgb = to_blend_factor(c.blendFacSrc, false);
    d.dstRgb = to_blend_factor(c.blendFacDst, true);
    break;
  case GX_BM_SUBTRACT:
    d.equation = GL_FUNC_REVERSE_SUBTRACT;
    d.srcRgb = GL_ONE;
    d.dstRgb = GL_ONE;
    break;
  case GX_BM_LOGIC:
    switch (c.blendOp) {
    case GX_LO_CLEAR:
      d.srcRgb = GL_ZERO;
      d.dstRgb = GL_ZERO;
      break;
    case GX_LO_NOOP:
      d.srcRgb = GL_ZERO;
      d.dstRgb = GL_ONE;
      break;
    default: // GX_LO_COPY and the logic ops upstream does not support
      break;
    }
    break;
  default: // GX_BM_NONE
    break;
  }
  if (c.dstAlpha == UINT32_MAX) {
    d.srcAlpha = d.srcRgb;
    d.dstAlpha = d.dstRgb;
  } else if (replace) {
    d.srcAlpha = GL_ONE; // the shader already wrote the constant
    d.dstAlpha = GL_ZERO;
  } else if (c.dstAlpha == 0) {
    d.srcAlpha = GL_ZERO;
    d.dstAlpha = GL_ZERO;
  } else {
    d.srcAlpha = GL_ONE; // alpha pre-pass wrote it; alpha is masked out in the main pass
    d.dstAlpha = GL_ZERO;
  }
  d.enable = d.equation != GL_FUNC_ADD || d.srcRgb != GL_ONE || d.dstRgb != GL_ZERO || d.srcAlpha != GL_ONE ||
             d.dstAlpha != GL_ZERO;
  return d;
}

PassState to_pass_state(const gx::PipelineConfig& c, bool replace) {
  PassState s;
  s.depthFunc = c.depthCompare ? to_compare(c.depthFunc) : GL_ALWAYS;
  s.depthMask = c.depthCompare && c.depthUpdate;
  switch (c.cullMode) {
  case GX_CULL_FRONT:
    s.cullFace = GL_FRONT;
    break;
  case GX_CULL_BACK:
    s.cullFace = GL_BACK;
    break;
  default:
    break;
  }
  s.blend = to_blend_desc(c, replace);
  s.colorMask[0] = s.colorMask[1] = s.colorMask[2] = c.colorUpdate;
  s.colorMask[3] = c.alphaUpdate;
  const float bias = std::bit_cast<float>(c.polygonOffsetBits);
  const float slope = std::bit_cast<float>(c.polygonOffsetScaleBits);
  s.polyBias = std::ceil(std::fabs(bias)) * (bias < 0.f ? -1.f : 1.f); // round away from zero, as upstream
  s.polyScale = slope;
  return s;
}

void apply_pass_state(const PassState& s) {
  const PassState& o = sCache.pass;
  const bool all = !sCache.passValid;
  set_enabled(GL_DEPTH_TEST, sCache.depthTest, true);
  if (all || o.depthFunc != s.depthFunc) {
    glDepthFunc(s.depthFunc);
  }
  if (all || o.depthMask != s.depthMask) {
    glDepthMask(s.depthMask ? GL_TRUE : GL_FALSE);
  }
  set_enabled(GL_CULL_FACE, sCache.cullEnabled, s.cullFace != 0);
  if (s.cullFace != 0 && (all || o.cullFace != s.cullFace)) {
    glCullFace(s.cullFace);
  }
  set_enabled(GL_BLEND, sCache.blendEnabled, s.blend.enable);
  if (s.blend.enable && (all || !(o.blend == s.blend))) {
    glBlendEquation(s.blend.equation);
    glBlendFuncSeparate(s.blend.srcRgb, s.blend.dstRgb, s.blend.srcAlpha, s.blend.dstAlpha);
  }
  if (all || o.colorMask[0] != s.colorMask[0] || o.colorMask[1] != s.colorMask[1] ||
      o.colorMask[2] != s.colorMask[2] || o.colorMask[3] != s.colorMask[3]) {
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
  }
  const bool offset = s.polyScale != 0.f || s.polyBias != 0.f;
  set_enabled(GL_POLYGON_OFFSET_FILL, sCache.polyOffset, offset);
  if (offset && (all || o.polyScale != s.polyScale || o.polyBias != s.polyBias)) {
    glPolygonOffset(s.polyScale, s.polyBias);
  }
  sCache.pass = s;
  sCache.passValid = true;
}

// ---- render targets ---------------------------------------------------------------------------

struct Target {
  GLuint fbo = 0;
  GLuint color = 0;
  GLuint depth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  Viewport viewport{};
  ClipRect scissor{};
  uint64_t lastUse = 0;
};

Target sEfb;
std::unordered_map<uint64_t, std::unique_ptr<Target>> sOffscreen;
Target* sCurrent = nullptr;
uint64_t sFrameIndex = 0;
uint32_t sPresentWidth = 960;
uint32_t sPresentHeight = 544;

void set_linear_clamp(GLuint texture) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool create_target(Target& t, uint32_t width, uint32_t height) {
  t.width = width;
  t.height = height;
  glGenTextures(1, &t.color);
  glBindTexture(GL_TEXTURE_2D, t.color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  set_linear_clamp(t.color);
  glGenRenderbuffers(1, &t.depth);
  glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
  glRenderbufferStorage(GL_RENDERBUFFER, DepthStencilFormat, static_cast<GLsizei>(width),
                        static_cast<GLsizei>(height));
  glGenFramebuffers(1, &t.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    Log.error("render target {}x{} incomplete (0x{:X})", width, height, static_cast<unsigned>(status));
  }
  t.viewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  t.scissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  return status == GL_FRAMEBUFFER_COMPLETE;
}

void destroy_target(Target& t) {
  if (t.fbo != 0) {
    glDeleteFramebuffers(1, &t.fbo);
  }
  if (t.color != 0) {
    glDeleteTextures(1, &t.color);
  }
  if (t.depth != 0) {
    glDeleteRenderbuffers(1, &t.depth);
  }
  t = {};
}

// Diagnostic: every distinct (target size, viewport, scissor) is logged once, so a clipped frame can
// be traced to the numbers that produced it instead of being guessed at from the picture.
bool log_rects_once(uint32_t tw, uint32_t th, const Viewport& v, const ClipRect& r) {
  static uint32_t lastTw = UINT32_MAX, lastTh = UINT32_MAX;
  static float lv[4] = {0, 0, 0, 0}, lr[4] = {0, 0, 0, 0};
  const float cv[4] = {v.left, v.top, v.width, v.height};
  const float cr[4] = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width),
                       static_cast<float>(r.height)};
  bool same = tw == lastTw && th == lastTh;
  for (int i = 0; i < 4 && same; ++i) {
    same = cv[i] == lv[i] && cr[i] == lr[i];
  }
  if (same) {
    return false;
  }
  lastTw = tw;
  lastTh = th;
  for (int i = 0; i < 4; ++i) {
    lv[i] = cv[i];
    lr[i] = cr[i];
  }
  return true;
}

void apply_viewport(const Target& t) {
  const Viewport& v = t.viewport;
  const auto round = [](float x) { return static_cast<GLint>(std::lround(x)); };
  if (log_rects_once(t.width, t.height, v, t.scissor)) {
    Log.warn("rect on {}x{}: viewport ({:.1f},{:.1f}) {:.1f}x{:.1f}  scissor ({},{}) {}x{}", t.width, t.height,
             v.left, v.top, v.width, v.height, t.scissor.x, t.scissor.y, t.scissor.width, t.scissor.height);
  }
  glViewport(round(v.left), round(v.top), std::max(round(v.width), 0), std::max(round(v.height), 0));
  glDepthRangef(std::clamp(v.znear, 0.f, 1.f), std::clamp(v.zfar, 0.f, 1.f));
}

void apply_scissor(const Target& t) {
  const ClipRect& r = t.scissor;
  const int32_t x = std::clamp<int32_t>(r.x, 0, static_cast<int32_t>(t.width));
  const int32_t y = std::clamp<int32_t>(r.y, 0, static_cast<int32_t>(t.height));
  const int32_t right = std::clamp<int32_t>(r.x + r.width, x, static_cast<int32_t>(t.width));
  const int32_t bottom = std::clamp<int32_t>(r.y + r.height, y, static_cast<int32_t>(t.height));
  glScissor(x, y, right - x, bottom - y);
}

void bind_target(Target& t) {
  sCurrent = &t;
  t.lastUse = sFrameIndex;
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  apply_viewport(t);
  set_enabled(GL_SCISSOR_TEST, sCache.scissorTest, true);
  apply_scissor(t);
}

// Clears color and/or depth of the current target through the scissor rectangle.
void clear_current(bool color, bool alpha, bool depth, const Vec4<float>& value, float depthValue, bool full) {
  set_enabled(GL_SCISSOR_TEST, sCache.scissorTest, !full);
  GLbitfield bits = 0;
  if (color || alpha) {
    glColorMask(color, color, color, alpha);
    glClearColor(value.x(), value.y(), value.z(), value.w());
    bits |= GL_COLOR_BUFFER_BIT;
  }
  if (depth) {
    glDepthMask(GL_TRUE);
    glClearDepthf(depthValue);
    bits |= GL_DEPTH_BUFFER_BIT;
  }
  if (bits != 0) {
    glClear(bits);
  }
  sCache.passValid = false; // color/depth masks changed behind the cache's back
  set_enabled(GL_SCISSOR_TEST, sCache.scissorTest, true);
}

Target* offscreen_target(uint32_t width, uint32_t height) {
  const uint64_t key = (uint64_t{width} << 32) | height;
  auto it = sOffscreen.find(key);
  if (it == sOffscreen.end()) {
    if (sOffscreen.size() >= MaxOffscreenTargets) {
      auto oldest = sOffscreen.begin();
      for (auto i = sOffscreen.begin(); i != sOffscreen.end(); ++i) {
        if (i->second->lastUse < oldest->second->lastUse) {
          oldest = i;
        }
      }
      if (oldest->second->lastUse != sFrameIndex) { // never evict a target used this frame
        destroy_target(*oldest->second);
        sOffscreen.erase(oldest);
      }
    }
    auto target = std::make_unique<Target>();
    create_target(*target, width, height);
    it = sOffscreen.emplace(key, std::move(target)).first;
  }
  return it->second.get();
}

// ---- full-screen programs: EFB copy conversion and present ------------------------------------

constexpr const char* QuadAttribs[] = {"aPos", "aUv"};
const float QuadVertices[] = {
    // x, y, u, v  (triangle strip; v runs top-down in "image" space)
    -1.f, -1.f, 0.f, 0.f, //
    1.f,  -1.f, 1.f, 0.f, //
    -1.f, 1.f,  0.f, 1.f, //
    1.f,  1.f,  1.f, 1.f, //
};

const char* QuadVertexSource = "attribute vec2 aPos;\n"
                               "attribute vec2 aUv;\n"
                               "varying vec2 vUv;\n"
                               "uniform vec4 uXf;\n" // offset.xy, scale.zw
                               "void main() {\n"
                               "  vUv = aUv * uXf.zw + uXf.xy;\n"
                               "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
                               "}\n";

const char* ConvPreamble = "precision highp float;\n"
                           "varying vec2 vUv;\n"
                           "uniform sampler2D uSrc;\n"
                           "uniform vec4 uFlags;\n" // x: force opaque alpha
                           "vec4 efb(vec2 uv) {\n"
                           "  vec4 c = texture2D(uSrc, uv);\n"
                           "  c.a = uFlags.x > 0.5 ? 1.0 : c.a;\n"
                           "  return c;\n"
                           "}\n"
                           "float intensity(vec3 rgb) { return dot(rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0; }\n"
                           "float q4(float v) { return floor(v * 16.0) / 15.0; }\n";

enum class Conv : uint8_t {
  Passthrough,
  Opaque,
  I4,
  I8,
  IA4,
  IA8,
  R4,
  RA4,
  RA8,
  A8,
  R8,
  G8,
  B8,
  RG8,
  GB8,
  Unsupported,
  Count,
};

Conv conversion_for(GXTexFmt format) {
  switch (format) {
  case GX_TF_I4:
    return Conv::I4;
  case GX_TF_I8:
    return Conv::I8;
  case GX_TF_IA4:
    return Conv::IA4;
  case GX_TF_IA8:
    return Conv::IA8;
  case GX_TF_RGB565:
    return Conv::Opaque;
  case GX_CTF_R4:
    return Conv::R4;
  case GX_CTF_RA4:
    return Conv::RA4;
  case GX_CTF_RA8:
    return Conv::RA8;
  case GX_CTF_A8:
    return Conv::A8;
  case GX_CTF_R8:
    return Conv::R8;
  case GX_CTF_G8:
    return Conv::G8;
  case GX_CTF_B8:
    return Conv::B8;
  case GX_CTF_RG8:
    return Conv::RG8;
  case GX_CTF_GB8:
    return Conv::GB8;
  case GX_TF_Z8:
  case GX_TF_Z16:
  case GX_TF_Z24X8:
  case GX_CTF_Z4:
  case GX_CTF_Z8M:
  case GX_CTF_Z8L:
  case GX_CTF_Z16L:
    return Conv::Unsupported; // depth cannot be sampled on vitaGL
  default:
    return Conv::Passthrough;
  }
}

const char* conversion_body(Conv conv) {
  switch (conv) {
  case Conv::Opaque:
    return "void main() { vec4 c = efb(vUv); gl_FragColor = vec4(c.rgb, 1.0); }\n";
  case Conv::I4:
    return "void main() { float i = q4(intensity(efb(vUv).rgb)); gl_FragColor = vec4(i, i, i, i); }\n";
  case Conv::I8:
    return "void main() { float i = intensity(efb(vUv).rgb); gl_FragColor = vec4(i, i, i, i); }\n";
  case Conv::IA4:
    return "void main() { vec4 c = efb(vUv); float i = q4(intensity(c.rgb)); gl_FragColor = vec4(i, i, i, q4(c.a)); }\n";
  case Conv::IA8:
    return "void main() { vec4 c = efb(vUv); float i = intensity(c.rgb); gl_FragColor = vec4(i, i, i, c.a); }\n";
  case Conv::R4:
    return "void main() { float r = q4(efb(vUv).r); gl_FragColor = vec4(r, r, r, r); }\n";
  case Conv::RA4:
    return "void main() { vec4 c = efb(vUv); float r = q4(c.r); gl_FragColor = vec4(r, r, r, q4(c.a)); }\n";
  case Conv::RA8:
    return "void main() { vec4 c = efb(vUv); gl_FragColor = vec4(c.r, c.r, c.r, c.a); }\n";
  case Conv::A8:
    return "void main() { float a = efb(vUv).a; gl_FragColor = vec4(a, a, a, a); }\n";
  case Conv::R8:
    return "void main() { float r = efb(vUv).r; gl_FragColor = vec4(r, r, r, r); }\n";
  case Conv::G8:
    return "void main() { float g = efb(vUv).g; gl_FragColor = vec4(g, g, g, g); }\n";
  case Conv::B8:
    return "void main() { float b = efb(vUv).b; gl_FragColor = vec4(b, b, b, b); }\n";
  case Conv::RG8:
    return "void main() { vec4 c = efb(vUv); gl_FragColor = vec4(c.r, c.r, c.r, c.g); }\n";
  case Conv::GB8:
    return "void main() { vec4 c = efb(vUv); gl_FragColor = vec4(c.g, c.g, c.g, c.b); }\n";
  case Conv::Unsupported:
    return "void main() { gl_FragColor = vec4(1.0); }\n";
  default: // Passthrough
    return "void main() { gl_FragColor = efb(vUv); }\n";
  }
}

struct QuadProgram {
  GLuint program = 0;
  GLint xf = -1;
  GLint src = -1;
  GLint flags = -1;
};
QuadProgram sConv[static_cast<size_t>(Conv::Count)];
QuadProgram sPresent;
bool sGlReady = false;
GLuint sFallbackTexture = 0;

// Fog range adjustment LUT, exposed as a 1-row RGBA texture to unit 15 (see Program::usesFogRange).
// The table's f32s live in frame.storage at byte offset `fogRangeBase * 4`; `width` is the render
// target width. Re-uploaded only when the (base, width) pair changes.
GLuint sFogRangeTexture = 0;
std::vector<uint8_t> sFogRangeRgba; // contents of sFogRangeTexture, to skip identical uploads
// Per-draw rebuild buffer for the fog-range LUT. bind_fog_range() runs for EVERY draw and used to
// construct a fresh std::vector<uint8_t> each time, which is a heap allocation per draw (~440/frame).
// Same pattern as sScratch: one buffer, cleared and reused, so the steady state does no allocation.
std::vector<uint8_t> sFogRangeScratch;

void finish_quad_program(QuadProgram& q) {
  if (q.program == 0) {
    return;
  }
  q.xf = glGetUniformLocation(q.program, "uXf");
  q.src = glGetUniformLocation(q.program, "uSrc");
  q.flags = glGetUniformLocation(q.program, "uFlags");
  use_program(q.program);
  if (q.src >= 0) {
    glUniform1i(q.src, 0);
  }
}

QuadProgram& conv_program(Conv conv) {
  QuadProgram& q = sConv[static_cast<size_t>(conv)];
  if (q.program == 0) {
    q.program = link_program(QuadVertexSource, std::string(ConvPreamble) + conversion_body(conv), QuadAttribs, 2,
                             "efb copy");
    finish_quad_program(q);
  }
  return q;
}

void draw_quad(const QuadProgram& q, float u0, float v0, float uScale, float vScale, bool opaque) {
  use_program(q.program);
  enable_attribs(0x3);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), QuadVertices);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), QuadVertices + 2);
  if (q.xf >= 0) {
    glUniform4f(q.xf, u0, v0, uScale, vScale);
  }
  if (q.flags >= 0) {
    glUniform4f(q.flags, opaque ? 1.f : 0.f, 0.f, 0.f, 0.f);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Puts GL into a known state for full-screen passes (no depth/blend/cull/scissor, all channels written).
void setup_fullscreen_state(bool scissor) {
  set_enabled(GL_DEPTH_TEST, sCache.depthTest, false);
  set_enabled(GL_CULL_FACE, sCache.cullEnabled, false);
  set_enabled(GL_BLEND, sCache.blendEnabled, false);
  set_enabled(GL_POLYGON_OFFSET_FILL, sCache.polyOffset, false);
  set_enabled(GL_SCISSOR_TEST, sCache.scissorTest, scissor);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);
  sCache.passValid = false;
}

void init_gl() {
  if (sGlReady) {
    return;
  }
  sGlReady = true;
  const auto gl_string = [](GLenum name) {
    const auto* text = reinterpret_cast<const char*>(glGetString(name));
    return text != nullptr ? text : "?";
  };
  Log.info("GL renderer '{}', vendor '{}', version '{}'", gl_string(GL_RENDERER), gl_string(GL_VENDOR),
           gl_string(GL_VERSION));
  // Present blit: the EFB is displayed as-is, so reuse the passthrough conversion.
  sPresent = {};
  glGenTextures(1, &sFallbackTexture);
  glBindTexture(GL_TEXTURE_2D, sFallbackTexture);
  const uint8_t white[4] = {255, 255, 255, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  invalidate_cache();
}

// Render scale for the EFB target. 75% means 456x336 for a 608x448 mode, upscaled to 960x544 at
// present time. This trades sharpness for fill rate, and it is the single cheapest lever on a
// tile-based GPU: cost is close to linear in covered pixels, so 75% is ~44% fewer fragments than 100%.
// It does NOT save draw-submit CPU work (draws are submitted after the copy), so measure it on device
// rather than assuming. DuskOnVita ships 75% as its default for the same reason: at 100% its house
// scene needed ~67 ms of GPU against ~43 ms at 75%.
// A/B measured in Vita3K, identical runs, same window ending at frame 1080 (71 programs, ~170 draws):
//   100% -> 151.94 ms/frame      75% -> ~113 ms/frame
// That is a 26% cut in wall time per frame for a 44% cut in fragments, so on this backend the frame cost
// is not purely fill-bound: the draw-submit and resolve work (11 per frame) is unchanged and dominates
// more as resolution falls. Emulator figure only; a console frame rate was never measured.
// The host e2e suite asserts exact pixels: a 1-px line, an 8-px point, a scissor edge, a TEV colour.
// Rendering those at 75 % and letting llvmpipe's filtering resolve them cannot reproduce the colour
// the test asks for (a full-intensity line becomes 0.588 of it), so the suite would have to be told a
// tolerance that also hides real regressions. On the host the scale buys nothing anyway -- it exists to
// save fill rate on a Cortex-A9 -- so the ratio is 100 % there and 75 % only on the Vita. This is what
// makes the suite a reference for the device rather than a second, differently-scaled renderer.
#ifdef __vita__
constexpr uint32_t kEfbScalePercent = 75;
#else
constexpr uint32_t kEfbScalePercent = 100;
#endif

void ensure_efb() {
  const auto size = main_target_size();
  const auto scaled = efb_target_size();
  const uint32_t width = scaled.x;
  const uint32_t height = scaled.y;
  // Measured once per target size: this is the number every viewport and scissor is scaled against,
  // and a wrong value here is indistinguishable from a clipping bug in a screenshot.
  if (sEfb.fbo == 0 || sEfb.width != width || sEfb.height != height) {
    Log.warn("EFB target: logical {}x{} -> {}x{} ({}%), present {}x{}", size.x, size.y, width, height,
             kEfbScalePercent, sPresentWidth, sPresentHeight);
  }
  if (sEfb.fbo != 0 && sEfb.width == width && sEfb.height == height) {
    return;
  }
  if (sEfb.fbo != 0) {
    Log.info("EFB resized {}x{} -> {}x{}", sEfb.width, sEfb.height, width, height);
    destroy_target(sEfb);
  }
  create_target(sEfb, width, height);
  sCache.boundTextureValid[0] = false; // texture names may be reused
}

// ---- GX programs ------------------------------------------------------------------------------

struct Program {
  GLuint program = 0;
  bool valid = false;
  bool replace = false;
  bool pnZero = false; // single matrix bank, valid only for draws with aPnMtxIdx == 0 (pn_mtx_provably_zero)
  uint32_t lights = gx::vita::MaxLightSlots; // light slots read (gx::vita::light_bucket); masks above are ignored
  gx::ShaderConfig config;
  gx::vita::DecodedLayout layout;
  uint16_t vsVec4 = 0;
  uint16_t fsVec4 = 0;
  std::vector<gx::vita::UniformSeg> vsSegs;
  std::vector<gx::vita::UniformSeg> fsSegs;
  bool usesExtra = false;
  bool usesFogRange = false;
  std::bitset<gx::MaxTextures> textures;
  GLint locVS = -1;
  GLint locFS = -1;
  GLint locExtra = -1;
  GLint locFogRangeSize = -1;
  GLint locTex[gx::MaxTextures];
  // Attribute locations as the driver assigned them (drivers may drop unused attributes and shift
  // the rest, so the bound index is not trusted); -1 = not used by the program.
  GLint attribLoc[gx::vita::MaxDecodedAttrs];
  // Uniform blobs are immutable within a frame, so (frame, offset) identifies what was uploaded last.
  uint64_t uniformFrame = UINT64_MAX;
  uint32_t uniformOffset = UINT32_MAX;
  float extra = -1.f;
};

std::unordered_map<HashType, std::unique_ptr<Program>> sPrograms;

// Where a draw's light masks sit in its uniform blob, cached per ShaderConfig (building the ShaderInfo for every
// draw would be far too slow).
struct LightLocEntry {
  gx::ShaderConfig config;
  gx::vita::LightMaskLocation loc;
};
std::unordered_map<HashType, LightLocEntry> sLightLocs;

const gx::vita::LightMaskLocation& light_location(const gx::PipelineConfig& config) {
  HashType seed = 0x4C49u; // distinct from the program-cache seeds
  while (true) {
    const HashType hash = xxh3_hash(config.shaderConfig, seed);
    const auto it = sLightLocs.find(hash);
    if (it == sLightLocs.end()) {
      const gx::ShaderInfo info = gx::build_shader_info(config.shaderConfig);
      return sLightLocs
          .emplace(hash, LightLocEntry{config.shaderConfig, gx::vita::light_mask_location(config.shaderConfig, info)})
          .first->second.loc;
    }
    if (it->second.config == config.shaderConfig) {
      return it->second.loc;
    }
    ++seed; // 32-bit hash collision between two different configs
  }
}

// A program's identity is the ShaderConfig plus three generator inputs that are not in it: whether it
// replaces destination alpha, whether it may carry a single matrix bank (the latter depends on the draw's
// GXSetCurrentMtx value, so the same ShaderConfig needs a second program for e.g. PNMTX1 draws), and how many
// light slots it reads (depends on the draw's light masks, see gx::vita::lights_in_use).
Program* get_program(const gx::PipelineConfig& config, bool replace, bool pnZero, uint32_t lights) {
  const HashType variant = (replace ? 1u : 0u) | (pnZero ? 2u : 0u) | (gx::vita::light_bucket_index(lights) << 2);
  HashType seed = variant;
  HashType hash;
  while (true) {
    hash = xxh3_hash(config.shaderConfig, seed);
    const auto it = sPrograms.find(hash);
    if (it == sPrograms.end()) {
      break;
    }
    Program& p = *it->second;
    if (p.replace == replace && p.pnZero == pnZero && p.lights == lights && p.config == config.shaderConfig) {
      return p.valid ? &p : nullptr;
    }
    seed += 32; // keeps the variant bits (5 used); 32-bit hash collision between two different programs
  }
  auto program = std::make_unique<Program>();
  program->replace = replace;
  program->pnZero = pnZero;
  program->lights = lights;
  program->config = config.shaderConfig;
  std::fill(std::begin(program->locTex), std::end(program->locTex), -1);
  std::fill(std::begin(program->attribLoc), std::end(program->attribLoc), -1);

  const gx::ShaderInfo info = gx::build_shader_info(config.shaderConfig);
  const gx::vita::ProgramSource source = gx::vita::build_program_source(config, info, pnZero, lights);
  program->layout = source.layout;
  program->vsVec4 = source.vsVec4;
  program->fsVec4 = source.fsVec4;
  program->vsSegs = source.vsSegs;
  program->fsSegs = source.fsSegs;
  program->usesExtra = source.usesExtra;
  program->usesFogRange = source.usesFogRange;
  program->textures = source.textures;

  const char* names[gx::vita::MaxDecodedAttrs];
  for (size_t i = 0; i < source.layout.count; ++i) {
    names[i] = source.layout.attrs[i].name;
  }
  // The label ties a log line, a dumped source file and a failure message to one program.
  char label[24];
  std::snprintf(label, sizeof(label), "gx-%08x", static_cast<uint32_t>(hash));
  Log.info("compiling {}: vertex {} B ({} vec4), fragment {} B ({} vec4), {} attributes", label, source.vertex.size(),
           source.vsVec4, source.fragment.size(), source.fsVec4, source.layout.count);
  const auto compileStart = std::chrono::steady_clock::now();
  program->program = link_program(source.vertex, source.fragment, names, source.layout.count, label);
  if (program->program != 0) {
    Log.info("{} ready in {} ms", label,
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - compileStart).count());
    program->valid = true;
    use_program(program->program);
    program->locVS = source.vsVec4 != 0 ? glGetUniformLocation(program->program, "uVS") : -1;
    program->locFS = source.fsVec4 != 0 ? glGetUniformLocation(program->program, "uFS") : -1;
    program->locExtra = source.usesExtra ? glGetUniformLocation(program->program, "uExtra") : -1;
    program->locFogRangeSize = source.usesFogRange ? glGetUniformLocation(program->program, "uFogRangeSize") : -1;
    if (source.usesFogRange) {
      const GLint locFogRange = glGetUniformLocation(program->program, "uFogRange");
      if (locFogRange >= 0) {
        use_program(program->program);
        glUniform1i(locFogRange, static_cast<GLint>(kFogRangeUnit));
      }
    }
    for (size_t i = 0; i < source.layout.count; ++i) {
      program->attribLoc[i] = glGetAttribLocation(program->program, source.layout.attrs[i].name);
    }
    for (uint32_t i = 0; i < gx::MaxTextures; ++i) {
      if (source.textures.test(i)) {
        char name[16];
        std::snprintf(name, sizeof(name), "uTex%u", i);
        program->locTex[i] = glGetUniformLocation(program->program, name);
        if (program->locTex[i] >= 0) {
          glUniform1i(program->locTex[i], static_cast<GLint>(i));
        }
      }
    }
  } else {
    Log.error("GX program failed; draws using shader config {:08x} are skipped", static_cast<uint32_t>(hash));
  }
  Program* result = program->valid ? program.get() : nullptr;
  sPrograms.emplace(hash, std::move(program));
  return result;
}

// ---- draws ------------------------------------------------------------------------------------

struct DrawScratch {
  std::vector<uint8_t> decoded;
  std::vector<gx::vita::ExpandedPrimitives> chunks;
  std::vector<float> uniforms; // stage uniform arrays assembled from the blob
};
DrawScratch sScratch;

// What one draw's decode produced, wherever it was produced: inline here (sScratch) or ahead of time
// on the decode worker. Taking this as a parameter instead of letting draw_pass read sScratch is what
// lets the overlapped and the fallback path share one code path, so a regression in one is a
// regression in both and gets caught by the same host suites.
struct DecodedView {
  const uint8_t* vertices = nullptr;
  const std::vector<gx::vita::ExpandedPrimitives>* chunks = nullptr;
};

void upload_uniforms(Program& p, const Frame& frame, const gx::DrawData& draw) {
  const bool sameBlob = p.uniformFrame == sFrameIndex && p.uniformOffset == draw.uniformRange.offset;
  if (!sameBlob) {
    p.uniformFrame = sFrameIndex;
    p.uniformOffset = draw.uniformRange.offset;
    const auto* blob = reinterpret_cast<const float*>(frame.uniforms.data() + draw.uniformRange.offset);
    const size_t blobVec4 = draw.uniformRange.size / 16;
    const auto upload = [&](GLint loc, uint16_t count, const std::vector<gx::vita::UniformSeg>& segs) {
      if (loc < 0 || count == 0) {
        return;
      }
      sScratch.uniforms.assign(static_cast<size_t>(count) * 4, 0.f);
      for (const auto& seg : segs) {
        if (static_cast<size_t>(seg.src) + seg.count > blobVec4 || seg.dst + seg.count > count) {
          continue; // malformed segment; leave zeros rather than read out of range
        }
        float* dst = sScratch.uniforms.data() + static_cast<size_t>(seg.dst) * 4;
        std::memcpy(dst, blob + static_cast<size_t>(seg.src) * 4, static_cast<size_t>(seg.count) * 16);
        gx::vita::convert_int_lanes(dst, seg.count, seg.intLanes);
      }
      glUniform4fv(loc, count, sScratch.uniforms.data());
    };
    upload(p.locVS, p.vsVec4, p.vsSegs);
    upload(p.locFS, p.fsVec4, p.fsSegs);
  }
  if (p.locExtra >= 0) {
    const float extra = draw.dstAlpha == UINT32_MAX ? 0.f : static_cast<float>(draw.dstAlpha) / 255.f;
    if (extra != p.extra) {
      p.extra = extra;
      glUniform4f(p.locExtra, extra, 0.f, 0.f, 0.f);
    }
  }
}

void bind_textures(const Program& p, const gx::DrawData& draw) {
  if (p.textures.none()) {
    return;
  }
  const TextureBindSet* set = find_bind_group(draw.bindGroups.textureBindGroup);
  for (uint32_t i = 0; i < gx::MaxTextures; ++i) {
    if (!p.textures.test(i)) {
      continue;
    }
    TextureRef* tex = set != nullptr ? set->slots[i].ref.get() : nullptr;
    if (tex == nullptr || tex->glTexture == 0) {
      bind_texture(i, sFallbackTexture);
      continue;
    }
    bind_texture(i, tex->glTexture);
    apply_sampler(*tex, set->slots[i].sampler);
  }
}

// A palette-derived texture (an EFB copy sampled through a TLUT: TP's field and dungeon maps) is computed on the GPU the
// first time a draw samples it after its copy or its TLUT changed. That pass changes the framebuffer, viewport, program,
// enables and bindings, so it runs before the draw sets anything up, and the cached state is dropped afterwards.
void refresh_derived_textures(const Program& p, const gx::DrawData& draw) {
  if (p.textures.none()) {
    return;
  }
  const TextureBindSet* set = find_bind_group(draw.bindGroups.textureBindGroup);
  if (set == nullptr) {
    return;
  }
  bool ranPass = false;
  for (uint32_t i = 0; i < gx::MaxTextures; ++i) {
    TextureRef* tex = p.textures.test(i) ? set->slots[i].ref.get() : nullptr;
    if (tex != nullptr && prepare_texture_for_sampling(*tex, sCurrent->fbo)) {
      ranPass = true;
    }
  }
  if (ranPass) {
    invalidate_cache();
    bind_target(*sCurrent);
  }
}

void set_vertex_pointers(const Program& p, const uint8_t* base) {
  uint32_t mask = 0;
  for (size_t i = 0; i < p.layout.count; ++i) {
    const GLint loc = p.attribLoc[i];
    if (loc < 0) {
      continue;
    }
    const auto& attr = p.layout.attrs[i];
    mask |= 1u << loc;
    if (attr.kind == gx::vita::DecodedAttr::Kind::UByte4Norm) {
      glVertexAttribPointer(static_cast<GLuint>(loc), 4, GL_UNSIGNED_BYTE, GL_TRUE, p.layout.stride,
                            base + attr.offset);
    } else {
      glVertexAttribPointer(static_cast<GLuint>(loc), attr.components, GL_FLOAT, GL_FALSE, p.layout.stride,
                            base + attr.offset);
    }
  }
  enable_attribs(mask);
}

// ---- frame profiler (HANDOVER 7.21) -----------------------------------------------------------
// Every percentage this port has ever quoted about its own frame was BORROWED from another port
// (7.14 quotes a Wii/PS2 profile's 42.9 % vertex prep / 20.1 % GX decode). That is an assumption about
// our shape, not a measurement of it. These counters answer "where does OUR frame actually go".
//
// Phase boundaries are chosen to be separable by what could move to another thread, because that is the
// decision the numbers have to inform:
//   decode   -- GX vertex decode / line expansion into sScratch. Pure CPU, no GL, candidate for a worker.
//   submit   -- the GL calls themselves (set_vertex_pointers + glDraw*). Must stay on the GL thread.
//   pass     -- uniform/texture/program binds around the draw (bind_fog_range, glUniform*, binds).
//   resolve  -- EFB copies.
//   other    -- everything else in replay() (init, clear, viewport/scissor, offscreen binds, surveys).
// Costs: two clock reads per draw. Measured once as a ratio of frame time, never as an absolute FPS.
struct PhaseTimes {
  double decodeMs = 0;
  double submitMs = 0;
  double passMs = 0;
  double resolveMs = 0;
  double otherMs = 0;
  uint32_t draws = 0;
};

PhaseTimes sPhase;       // this frame
PhaseTimes sPhaseWindow; // accumulated over the reporting window

// `other` is the remainder of replay() after the four draw phases. These break it down PER FRAME (they are
// reset each frame), so a frame's split and its `otherMs` always agree.
// `other` was 60 % of a gameplay frame, which is too large to leave as one bucket. These split it so the
// next question has an answer: is it the frame's own setup, the clear, or the diagnostic batching survey
// (which allocates hash maps over every draw and is NOT shipped code -- if the survey is the bulk of
// `other`, then `other` is an artefact of this instrumentation and the real frame is the other 40 %).
double sOtherSetupMs = 0;  // init_gl, flush_texture_deletions, ensure_efb, invalidate_cache
double sOtherClearMs = 0;  // bind_target + clear_current
double sOtherSurveyMs = 0; // the 7.3c batching survey, diagnostic only
double sOtherTailMs = 0;   // per-op viewport/scissor/offscreen binds, and anything left over
// The four above are PER FRAME (reset at the top of replay()). The report runs later in the same call,
// after that reset, so reading them there always printed 0.0 % -- three columns of confident nothing.
// These are the end-of-frame snapshots the report actually prints.
double sLastSetupMs = 0;
double sLastClearMs = 0;
double sLastTailMs = 0;

// ---- `pass` split (HANDOVER 7.25) --------------------------------------------------------------
// `pass` is the largest phase in both measurements (38.4 % of a gameplay frame in 7.21, 56-72 % on the
// benchmark) and has never been attributed. These split it by the five things draw_pass() does per draw,
// so the next question ("can any of it be removed?") has an answer instead of a guess.
//
// The expected finding is stated BEFORE measuring, so it cannot be retro-fitted: most of the underlying
// gl* calls are already suppressed by GlCache (use_program, bind_texture, set_enabled, and
// upload_uniforms' sameBlob check), so a large part of `pass` should be the CPU cost of the CHECKING
// rather than of driver calls. If that is what the numbers say, the lever is fewer checks per draw
// (fatter draws, merged state), not cheaper GL calls, and no GL micro-optimisation will move it.
//
// Read as a share of `pass`, not of the frame: the parts sum to `pass`, and each is named after the call
// in draw_pass() that it wraps.
struct PassSplit {
  double programMs = 0; // get_program + use_program (+ refresh_derived_textures)
  double stateMs = 0;   // apply_pass_state
  double uniformMs = 0; // upload_uniforms
  double textureMs = 0; // bind_textures
  double fogMs = 0;     // bind_fog_range, including its LUT rebuild
  uint32_t passes = 0;  // draw_pass() calls, so a 2-pass alpha draw counts 2
};
PassSplit sPass;       // this frame
PassSplit sPassWindow; // this window
PassSplit sLastPass;   // end-of-frame snapshot, same reason as sLastSetupMs
// How many binds actually reached gl*, as opposed to being suppressed by a cache.
uint64_t sTextureBindsWindow = 0;
uint64_t sFogUploadsWindow = 0;

inline double ms_since(std::chrono::steady_clock::time_point a) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

// ---- decode worker (HANDOVER 7.22) --------------------------------------------------------------
// 36.9 % of a measured gameplay frame is GX vertex decode, and decode is a PURE function of
// (PipelineConfig, DrawData, Frame): no GL call, no global state (vita_vertex.cpp compiles its cache
// bookkeeping out when the cache is off), and no dependence on draw ORDER. That is exactly the shape
// that can run on another core while this one talks to vitaGL, so it does.
//
// Shape: at the top of replay() the frame's whole draw list is published. One long-lived worker
// decodes it in order into a small ring of reusable slots; the GL thread walks the op list as before
// and picks up each result when it needs it. Decode (36.9 %) therefore overlaps submit+pass (53.1 %),
// and because the consumer outpaces the producer per draw (0.53 vs 0.37 of a frame) the pipeline
// stays full after the first draw or two.
//
// Four deliberate choices, each because the alternative is worse:
//  * ONE thread, created once and DETACHED -- not one per frame, and not joined. Per-frame creation is
//    what the 7.1 abort history is about; a joinable std::thread whose destructor runs would call
//    std::terminate, and a worker parked on a condition variable costs nothing at teardown.
//  * A BOUNDED ring of slots, so an unexpectedly heavy frame cannot balloon the heap. The consumer
//    frees a slot as soon as it has issued the draw, which is what keeps the ring from stalling.
//  * A WAIT, never an inline decode, when a result is not ready. The consumer only ever wants the
//    oldest undelivered draw, so the producer is guaranteed to reach it; decoding inline instead
//    would steal the core the worker is using and make the frame slower than having no worker.
//  * Any failure (the thread not starting, a slot never filled) falls back to the old path rather
//    than deadlocking: the counters are reported every window so a silent fallback is visible.
constexpr uint32_t kDecodeSlots = 24;

struct DecodeJob {
  uint32_t drawIndex = 0;
  const gx::PipelineConfig* config = nullptr;
  bool lineMode = false;
};

struct DecodeSlot {
  std::vector<uint8_t> decoded;
  std::vector<gx::vita::ExpandedPrimitives> chunks;
};

// The handshake is THREE MONOTONE COUNTERS, not a lock per draw.
//
// Measured, and the reason this is not a friendly mutex: the first version locked a std::mutex and
// signalled a condition variable on every draw. At 393 draws per frame that is ~1,600 futex operations
// per frame, and it made the frame 22 % SLOWER while the wait itself was only 0.2 % -- the overhead
// swamped the overlap it existed to create. Counters cost one atomic load per draw instead.
//
//   readyCount  -- jobs whose slot is filled. Written by the worker, read by the GL thread.
//   freeCount   -- jobs whose slot has been released. Written by the GL thread, read by the worker.
//   nextJob     -- the worker's own cursor. Touched by the worker only, so it needs no atomics.
//
// Both sides proceed strictly in ordinal order (the worker decodes in publish order, the op loop
// consumes in the same order), so a counter IS the whole protocol. The mutex and the condition
// variables are used ONLY on the blocking path -- a handful of times per run -- which is why they cost
// nothing in the steady state.
struct DecodeWorker {
  std::mutex mu;
  std::condition_variable cvNewJob;  // worker -> waiting for the next frame's jobs
  std::condition_variable cvSlotFree; // worker -> waiting for the consumer to release a slot
  std::condition_variable cvReady;    // GL thread -> waiting for a specific decode
  const Frame* frame = nullptr;       // owned by the GL thread; valid until it drains
  std::vector<DecodeJob> jobs;
  uint32_t jobCount = 0;
  uint32_t nextJob = 0; // worker only
  // Each hot counter owns a cache line. This did NOT fix the `pass` inflation (measured: 26 % before and
  // after), so false sharing was not the cause -- it is kept because two threads genuinely write these
  // and it costs nothing, but the finding it was meant to explain is recorded below under
  // `waiters`/`cvSlotFree`, which is where the real cost was.
  static constexpr size_t kCacheLine = 64;
  alignas(kCacheLine) std::atomic<uint32_t> readyCount{0}; // worker writes, GL thread reads
  char readyPad[kCacheLine - sizeof(std::atomic<uint32_t>)]{};
  alignas(kCacheLine) std::atomic<uint32_t> freeCount{0}; // GL thread writes, worker reads
  char freePad[kCacheLine - sizeof(std::atomic<uint32_t>)]{};
  // THE ACTUAL COST (HANDOVER 7.25). `decode_release` called cvSlotFree.notify_one() on EVERY draw, and a
  // notify on a condition variable with no waiter still takes the internal lock and looks at the wait
  // list. Measured by timing just the release: 24-29 % of the whole frame over 47,200 calls, ~0.5 us
  // each -- which is not a decode, it is the notify. The fix is to not notify when nobody is parked.
  // `waiters` counts threads blocked in cvSlotFree; the GL thread only signals above zero. Padded
  // because it is read by both sides on the hot path.
  alignas(kCacheLine) std::atomic<uint32_t> waiters{0};
  char waitersPad[kCacheLine - sizeof(std::atomic<uint32_t>)]{};
  DecodeSlot slots[kDecodeSlots];
  bool running = false;
  // Reported every window, then reset. `decodeMs` is the worker's own decode time: the answer to "did
  // the work actually happen somewhere else, or did it vanish?".
  double decodeMs = 0;
  uint32_t decodedDraws = 0;
  // Hand-off accounting (7.25): how many releases happened and how many of them signalled the worker.
  // `slotNotifies` far below `releaseCalls` is the point of the change; equal to it means the worker
  // really is parking on most draws and the notify is unavoidable.
  uint32_t releaseCalls = 0;
  uint32_t slotNotifies = 0;
  uint32_t inlineDecodes = 0;
  uint32_t waitCalls = 0;
  uint32_t waitBlocks = 0;
  std::thread thread;
};
// Intentionally leaked. The worker is detached and parks on a condition variable; if its storage had
// static duration, the variable would be destroyed while the thread is still waiting on it during
// static teardown (the e2e harness exits without calling shutdown_gl). A reference to an immortal
// object is trivial to destroy, so there is nothing left to race with.
DecodeWorker& sWorker = *new DecodeWorker();
std::vector<uint32_t> sOrdinalOfDraw; // draw index -> job ordinal, or UINT32_MAX if not posted
// Reused across frames: a full scene's job list is ~1.7 KiB and this runs every frame, so reallocating
// it would be exactly the per-frame churn the rest of this file goes out of its way to avoid.
std::vector<DecodeJob> sPublishJobs;
std::vector<uint32_t> sPublishOrdinals;

// Byte-exact A/B proof that the worker's output IS the inline output. Opt-in (TPV_VERIFY_DECODE=1),
// because while it ran by default it double-decoded in the worker-on build only, and that made every
// A/B measure the verifier instead of the change. It has already done its job: on the corrected
// benchmark it compared 23,600 worker decodes against inline decodes and 0 differed.
uint32_t sVerifyFrames = 0;
uint32_t sVerifyChecks = 0;
uint32_t sVerifyDiffs = 0;
uint32_t sVerifySkipped = 0;
std::vector<uint8_t> sVerifyScratch;
std::vector<gx::vita::ExpandedPrimitives> sVerifyChunks;

void decode_worker_main() {
#ifdef __vita__
  // Register for the census and take the second user core. This is the one place in the port that
  // deliberately moves work to a specific core: the GL thread owns User0, so the decode worker goes to
  // User1 (see vita/src/cpu_placement.hpp). Nothing here is fatal if the registry is full or the mask is
  // refused -- wanted_mask/pin_all degrade, and the thread runs wherever the scheduler puts it.
  const int cpuHandle = tpv::cpu::register_thread("decode", tpv::cpu::Role::Decode, tpv::cpu::Core::User1);
  if (tpv::cpu::pinned() && cpuHandle >= 0) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), tpv::cpu::wanted_mask(cpuHandle));
  }
#endif
  std::unique_lock<std::mutex> lock(sWorker.mu);
  for (;;) {
    sWorker.cvNewJob.wait(lock, [] { return sWorker.nextJob < sWorker.jobCount; });
    const uint32_t ordinal = sWorker.nextJob;
    // Wait for this slot to be released before overwriting it. With 24 slots and a consumer that is
    // faster than the producer, this is only reached when the frame has more than 24 draws in flight.
    //
    // TIME-BOUNDED, and in a LOOP, and both are required. With coarse release the GL thread publishes
    // once per kReleaseStride draws, so waiting forever on a notify would stall a whole stride if one
    // signal were missed. But `wait_for` RETURNS ON TIMEOUT with the predicate still false -- the first
    // version used a bare `wait_for` and simply carried on, so after a 200 us timeout the worker
    // overwrote a slot the consumer was still reading. The verifier caught it precisely: 29 of 23,600
    // decoded results differed, 0.12 %, the rate you would expect from an occasional timeout, while the
    // frame looked faster (release 24 % -> 2.8 %). A timeout must mean "check again", never "proceed".
    sWorker.waiters.fetch_add(1, std::memory_order_seq_cst);
    while (ordinal >= sWorker.freeCount.load(std::memory_order_acquire) + kDecodeSlots) {
      // The predicate is the one that means "a slot is free", i.e. the condition the outer `while`
      // waits for. (The first version negated it, which made every call return immediately and
      // busy-spin for the whole stride.)
      sWorker.cvSlotFree.wait_for(lock, std::chrono::milliseconds(1), [ordinal] {
        return ordinal < sWorker.freeCount.load(std::memory_order_acquire) + kDecodeSlots;
      });
    }
    sWorker.waiters.fetch_sub(1, std::memory_order_release);
    const DecodeJob job = sWorker.jobs[ordinal];
    const Frame* frame = sWorker.frame;
    lock.unlock();
    // Off the lock: this is the whole point of the thread. Nothing below touches GL state or any global
    // the GL thread writes -- that is the invariant the 7.21 measurement bought, and 23,600 byte-exact
    // comparisons say it holds.
    DecodeSlot& slot = sWorker.slots[ordinal % kDecodeSlots];
    const auto start = std::chrono::steady_clock::now();
    const gx::DrawData& draw = frame->draws[job.drawIndex];
    const gx::vita::DecodedLayout layout = gx::vita::decoded_layout(job.config->shaderConfig);
    if (job.lineMode) {
      gx::vita::expand_lines(*job.config, draw, *frame, layout, slot.chunks);
    } else {
      const size_t bytes = static_cast<size_t>(draw.vtxCount) * layout.stride;
      // Do not let one oversized draw pin its slot's capacity for the rest of the run.
      if (slot.decoded.capacity() > 4 * bytes + 65536) {
        std::vector<uint8_t>().swap(slot.decoded);
      }
      slot.decoded.resize(bytes);
      gx::vita::decode_draw(*job.config, draw, *frame, layout, slot.decoded);
    }
    const double ms = ms_since(start);
    ++sWorker.nextJob;
    sWorker.decodeMs += ms; // worker thread only, so no lock; read under the lock at report time
    ++sWorker.decodedDraws;
    // Publish LAST, release-ordered: the slot's contents must be visible before the counter says so.
    sWorker.readyCount.store(ordinal + 1, std::memory_order_release);
    // notify_one, not notify_all: there is exactly one consumer. At 393 draws per frame, notify_all
    // walks the whole wait list once per draw for a single waiter.
    sWorker.cvReady.notify_one();
    lock.lock();
  }
}

// A/B control for the ONE question that matters here: does the overlap actually shorten the frame?
// The off-thread percentage only proves the WORK moved; it cannot prove the frame got faster, and a
// second thread competing for the same core can move work without saving time. The env var is for the
// host harness, the marker file for a console run.
bool decode_worker_allowed() {
  static const bool allowed = [] {
    // OFF BY DEFAULT, and this is the important line in this file. HANDOVER 7.22 measured the overlap on
    // a corrected benchmark (47,200 draws, 393 per frame, byte-identical recorded frame each iteration):
    //   worker off  median 48.2 ms      worker on  median 56.2 ms      = 16 % SLOWER
    // with a rendezvous of only 0.1 % of the frame and 0.17 % of draws ever blocking. The mechanism
    // works -- decode left the critical path, which is what it was built to do -- but it costs more
    // than it saves, because on that frame decode is only 11 % of the time while the hand-off and a
    // second runnable thread on a 16-thread llvmpipe rasteriser cost more than 11 %.
    //
    // It is kept behind a flag rather than deleted because the proxy is not the target. The Vita is a
    // quad-core Cortex-A9 and a game may use three cores (the 4th is reserved for system tasks;
    // CapUnlocker can release it), so a spare core exists here -- but it is ONE contended core, not the
    // fifteen idle ones this host has, and the point of the console number is that decode is 37-43 % of
    // a real frame there (7.20/7.21) rather than the 11 % measured on the benchmark. What would settle
    // it is one run on hardware with the flag on and off. Until then this is not shipped behaviour.
    // Presence is not the question, the VALUE is. The first version tested `getenv(...) == nullptr`, so
    // `TPV_ENABLE_DECODE_WORKER=0` ENABLED the worker: an A/B "control" arm that ran the feature under
    // test and reported 2 threads while claiming to be off. Accept 0/off/no/false/empty as disabled.
    const char* flag = std::getenv("TPV_ENABLE_DECODE_WORKER");
    if (flag == nullptr) {
      return false;
    }
    if (flag[0] == '\0' || flag[0] == '0' || flag[0] == 'n' || flag[0] == 'N' || flag[0] == 'f' ||
        flag[0] == 'F') {
      return false;
    }
    const char* path = "ux0:data/tpvita/no_decode_worker";
    if (std::FILE* f = std::fopen(path, "rb")) {
      std::fclose(f);
      Log.warn("decode worker: DISABLED by {} (A/B control); decoding inline", path);
      return false;
    }
    return true;
  }();
  return allowed;
}

void decode_worker_start() {
  if (sWorker.running || !decode_worker_allowed()) {
    return;
  }
  if (std::getenv("TPV_VERIFY_DECODE") != nullptr) {
    sVerifyFrames = 60;
  }
  try {
    sWorker.thread = std::thread(decode_worker_main);
    sWorker.thread.detach();
    sWorker.running = true;
    Log.info("decode worker: started, {} slots; decode runs off the GL thread", kDecodeSlots);
  } catch (const std::exception& e) {
    // A port must not lose the picture because an optimisation could not start.
    sWorker.running = false;
    Log.warn("decode worker: could not start ({}); decoding inline as before", e.what());
  }
}

// Publishes this frame's draws. Runs on the GL thread, before the op loop, so the worker has the whole
// frame's work queued while the GL thread is still doing setup and the first passes. Which draws are
// worth posting is decided HERE and not on the worker, both because find_pipeline() reads a map only
// this thread may touch and because the consumer must receive exactly the draws that draw_op() will
// process -- a job nobody consumes would keep its slot busy for ever.
void decode_publish(Frame& frame) {
  decode_worker_start();
  if (!sWorker.running) {
    return;
  }
  sPublishJobs.clear();
  sPublishOrdinals.assign(frame.draws.size(), UINT32_MAX);
  for (const Op& op : frame.ops) {
    if (op.kind != Op::Kind::Draw) {
      continue;
    }
    const gx::DrawData& draw = frame.draws[op.index];
    const gx::PipelineConfig* config = find_pipeline(draw.pipeline);
    if (config == nullptr || draw.vtxCount == 0) {
      continue;
    }
    const bool lineMode = config->shaderConfig.lineMode != 0;
    if (!lineMode && draw.indexCount == 0 && draw.vtxCount < 3) {
      continue;
    }
    sPublishOrdinals[op.index] = static_cast<uint32_t>(sPublishJobs.size());
    sPublishJobs.push_back({op.index, config, lineMode});
  }
  {
    std::lock_guard<std::mutex> lock(sWorker.mu);
    sWorker.frame = &frame;
    sWorker.jobs = sPublishJobs;
    sWorker.jobCount = static_cast<uint32_t>(sWorker.jobs.size());
    sWorker.nextJob = 0;
    sWorker.readyCount.store(0, std::memory_order_release);
    sWorker.freeCount.store(0, std::memory_order_release);
  }
  sOrdinalOfDraw.swap(sPublishOrdinals);
  sWorker.cvNewJob.notify_all();
}

// Blocks until this draw's decode result exists. The time spent actually blocking is the part of decode
// that did NOT move off the critical path, so it is booked as `decode` alongside the inline path.
const DecodeSlot* decode_wait(uint32_t ordinal, uint32_t drawIndex) {
  (void)drawIndex;
  if (!sWorker.running || ordinal == UINT32_MAX) {
    return nullptr;
  }
  ++sWorker.waitCalls;
  // Fast path: one acquire load, no lock. This is the normal case (>99 % of draws, measured).
  if (sWorker.readyCount.load(std::memory_order_acquire) <= ordinal) {
    ++sWorker.waitBlocks;
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(sWorker.mu);
    sWorker.cvReady.wait(lock, [ordinal] {
      return sWorker.readyCount.load(std::memory_order_acquire) > ordinal;
    });
    sPhase.decodeMs += ms_since(start);
  }
  return &sWorker.slots[ordinal % kDecodeSlots];
}

// COARSE RELEASE (HANDOVER 7.25). The measured finding that forced this: `decode_release` was 24-29 %
// of the whole frame over 47,200 calls, and 88-92 % of those calls found the worker PARKED and had to
// signal it -- so it was not a wasted notify, it was 47,200 wake-ups per frame. Gating the notify on a
// waiter counter changed nothing (23.9 % before, 23.9 % after).
//
// The fix is the one the whole exercise converged on: hand off a RANGE, not an item. The GL thread now
// publishes every kReleaseStride-th release, so the worker is woken ~1/16th as often. `freeCount` here
// means "jobs whose slot the CONSUMER has finished with", and the worker's lag bound becomes
// kDecodeSlots - kReleaseStride, which is still a hard bound, so the ring cannot be overrun. Correctness
// does not depend on a tight bound: the consumer releases only after it has issued the draw's GL calls,
// and the worker never touches a slot it has not been given an ordinal for.
constexpr uint32_t kReleaseStride = 16;
static_assert(kReleaseStride < kDecodeSlots, "the worker must have at least one slot it can run ahead by");

void decode_release(uint32_t ordinal) {
  ++sWorker.releaseCalls;
  // ONE store per stride, and no per-draw fallback. The first version of this had a "publish now if
  // someone is parked" branch, and it did nothing: the worker is parked on 91 % of draws (measured), so
  // the fallback fired on almost every call and the stride never applied. The parked worker now waits
  // with a short timeout instead, so it re-checks on its own and the stride actually holds.
  if (((ordinal + 1) % kReleaseStride) != 0) {
    return;
  }
  // Strictly increasing: the op loop consumes ordinals in publish order, so a plain store is correct
  // and needs no read-modify-write.
  sWorker.freeCount.store(ordinal + 1, std::memory_order_release);
  if (sWorker.waiters.load(std::memory_order_relaxed) != 0) {
    sWorker.cvSlotFree.notify_one();
    ++sWorker.slotNotifies;
  }
}

// HANDOVER 7.25: `pass` is measured as an ENCLOSING span (whole draw_pass minus the submit timer), so
// anything this thread blocks on while inside it is booked to `pass`. With a worker running, that
// includes the hand-off. These two probes tell the two possible causes apart instead of assuming one:
// RELEASE = time spent publishing a finished slot, WAIT_NOTIFY = time spent in the condition-variable
// notify that wakes the worker. If neither is large, the cost is not the hand-off at all and is more
// likely cache traffic from two threads touching neighbouring data, which no timer can attribute.
double sReleaseMs = 0;    // per frame, inside the `pass` span
double sNotifyMs = 0;     // per frame, inside the `pass` span
uint32_t sReleaseCalls = 0;
void decode_release_timed(uint32_t ordinal) {
  const auto start = std::chrono::steady_clock::now();
  decode_release(ordinal);
  sReleaseMs += ms_since(start);
  ++sReleaseCalls;
}

// End of frame: every posted draw has been consumed and released by now, so this is a formality that
// also makes the next frame's publish safe (the worker must not still be reading `jobs` when the next
// frame swaps it). Counted in `other` if it ever costs anything measurable.
// Decodes the same draw inline and compares. Called only while sVerifyFrames > 0 (TPV_VERIFY_DECODE).
void decode_verify(const Frame& frame, const gx::DrawData& draw, const gx::PipelineConfig& config, bool lineMode,
                   const DecodeSlot& slot) {
  const gx::vita::DecodedLayout layout = gx::vita::decoded_layout(config.shaderConfig);
  bool same;
  if (lineMode) {
    gx::vita::expand_lines(config, draw, frame, layout, sVerifyChunks);
    same = sVerifyChunks.size() == slot.chunks.size();
    for (size_t i = 0; same && i < sVerifyChunks.size(); ++i) {
      same = sVerifyChunks[i].vertices == slot.chunks[i].vertices && sVerifyChunks[i].indices == slot.chunks[i].indices;
    }
    ++sVerifySkipped;
  } else {
    sVerifyScratch.resize(static_cast<size_t>(draw.vtxCount) * layout.stride);
    gx::vita::decode_draw(config, draw, frame, layout, sVerifyScratch);
    same = sVerifyScratch.size() == slot.decoded.size() &&
           (sVerifyScratch.empty() ||
            std::memcmp(sVerifyScratch.data(), slot.decoded.data(), sVerifyScratch.size()) == 0);
  }
  ++sVerifyChecks;
  if (!same) {
    ++sVerifyDiffs;
  }
}

// End of frame: every posted draw has been consumed and released by now, so this is a formality that
// also makes the next frame's publish safe (the worker must not still be reading `jobs` when the next
// frame replaces it). Counted in `other` if it ever costs anything measurable.
void decode_drain() {
  if (!sWorker.running) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(sWorker.mu);
    // Make the predicate satisfiable: with coarse release (kReleaseStride) the last partial stride may
    // never have published, so the wait below would sleep forever. Publishing jobCount here is safe --
    // every draw has already been consumed by the op loop, which is the only thing that calls drain.
    if (sWorker.freeCount.load(std::memory_order_acquire) < sWorker.jobCount) {
      sWorker.freeCount.store(sWorker.jobCount, std::memory_order_release);
      sWorker.cvSlotFree.notify_all();
    }
    sWorker.cvSlotFree.wait(lock, [] {
      return sWorker.freeCount.load(std::memory_order_acquire) >= sWorker.jobCount;
    });
  }
  sOtherTailMs += ms_since(start);
}

// submit_geometry is the only place that issues glDraw*, so its time IS the submit phase.
void submit_geometry(const Program& p, const uint8_t* vertices, uint32_t vertexCount, const uint16_t* indices,
                     uint32_t indexCount) {
  const auto submitStart = std::chrono::steady_clock::now();
  struct SubmitTimer {
    std::chrono::steady_clock::time_point start;
    ~SubmitTimer() { sPhase.submitMs += ms_since(start); }
  } submitTimer{submitStart};
  set_vertex_pointers(p, vertices);
  if (indices != nullptr) {
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_SHORT, indices);
    return;
  }
  // glDrawArrays reads a fixed-size built-in index buffer on vitaGL: split, on triangle boundaries.
  for (uint32_t first = 0; first < vertexCount;) {
    const uint32_t count = std::min<uint32_t>(vertexCount - first, MaxArrayVertices);
    if (first != 0) {
      set_vertex_pointers(p, vertices + static_cast<size_t>(first) * p.layout.stride);
    }
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
    first += count;
  }
}

// Uploads the fog-range LUT (frame.storage @ fogRangeBase*4, one float per column of the render target
// the draw goes to) into a 1-row RGBA8 texture on unit kFogRangeUnit, NEAREST, so texel x == column x.
// Factors are >= 1 (see fog_range_encode in vita_gx.hpp), so they are stored as 16-bit fixed point.
void bind_fog_range(const Frame& frame, const gx::DrawData& draw, const Program& p) {
  if (!p.usesFogRange) {
    return;
  }
  // The record-time table has one entry per column of get_render_target_size().x; replaying into the
  // same target, that is the current target's width. fogRangeBase 0 is a valid offset (first storage
  // push of the frame), so it is not a "no table" marker here: the program uses a table, the draw has one.
  const uint32_t width = sCurrent != nullptr ? sCurrent->width : sEfb.width;
  const size_t byteBase = static_cast<size_t>(draw.immediateData.fogRangeBase) * sizeof(uint32_t);
  const size_t bytes = static_cast<size_t>(width) * sizeof(float);
  if (width == 0 || byteBase > frame.storage.size() || bytes > frame.storage.size() - byteBase) {
    Log.warn("fog range LUT out of range (base {} width {}, storage {} bytes)", draw.immediateData.fogRangeBase, width,
             frame.storage.size());
    return;
  }
  // Reuse the buffer instead of constructing a vector per draw: this runs for every draw that has fog
  // range adjustment enabled (~440 draws/frame), so a per-call allocation is pure waste. resize() does
  // not shrink capacity, so after the first few draws this never allocates again.
  sFogRangeScratch.resize(static_cast<size_t>(width) * 4);
  for (uint32_t x = 0; x < width; ++x) {
    float factor;
    std::memcpy(&factor, frame.storage.data() + byteBase + static_cast<size_t>(x) * sizeof(float), sizeof(float));
    uint8_t* texel = &sFogRangeScratch[static_cast<size_t>(x) * 4];
    gx::vita::fog_range_encode(factor, texel[0], texel[1]);
    texel[2] = 0;
    texel[3] = 255;
  }
  if (sFogRangeTexture == 0) {
    glGenTextures(1, &sFogRangeTexture);
    glBindTexture(GL_TEXTURE_2D, sFogRangeTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  // Storage offsets repeat from frame to frame, so (base, width) does not identify the contents: compare them.
  if (sFogRangeRgba != sFogRangeScratch) {
    ++sFogUploadCount;
    glBindTexture(GL_TEXTURE_2D, sFogRangeTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 sFogRangeScratch.data());
    sFogRangeRgba = sFogRangeScratch;
  }
  bind_texture(kFogRangeUnit, sFogRangeTexture);
  if (p.locFogRangeSize >= 0) {
    glUniform2f(p.locFogRangeSize, 1.f / static_cast<float>(width), 0.f);
  }
}

// One program pass of a draw; `decoded` / `chunks` are already filled for this draw.
void draw_pass(const Frame& frame, const gx::DrawData& draw, const gx::PipelineConfig& config, bool lineMode,
               const DecodedView& decoded) {
#if defined(TPV_PASS_SPLIT)
  // Five timers, two clock reads each, so `pass` can be attributed (HANDOVER 7.25). Off by default: this
  // is diagnostic instrumentation and HANDOVER trap 31 / measurement.md 2.7 is what happens when an A/B
  // arm contains instrumentation the other arm does not.
  const auto tProgram = std::chrono::steady_clock::now();
#endif
  const bool replace = replaces_alpha(config);
  const uint32_t lights = gx::vita::lights_in_use(light_location(config),
                                                  frame.uniforms.data() + draw.uniformRange.offset,
                                                  draw.uniformRange.size);
  Program* program = get_program(config, replace,
                                 gx::vita::pn_mtx_provably_zero(config.shaderConfig, draw.immediateData.currentPnMtx),
                                 lights);
  if (program == nullptr) {
    return;
  }
  refresh_derived_textures(*program, draw);
  use_program(program->program);
#if defined(TPV_PASS_SPLIT)
  sPass.programMs += ms_since(tProgram);
  const auto tState = std::chrono::steady_clock::now();
#endif
  apply_pass_state(to_pass_state(config, replace));
#if defined(TPV_PASS_SPLIT)
  sPass.stateMs += ms_since(tState);
  ++sPass.passes;
  const auto tUniform = std::chrono::steady_clock::now();
#endif
  upload_uniforms(*program, frame, draw);
#if defined(TPV_PASS_SPLIT)
  sPass.uniformMs += ms_since(tUniform);
  const auto tTexture = std::chrono::steady_clock::now();
#endif
  bind_textures(*program, draw);
#if defined(TPV_PASS_SPLIT)
  sPass.textureMs += ms_since(tTexture);
  const auto tFog = std::chrono::steady_clock::now();
#endif
  bind_fog_range(frame, draw, *program);
#if defined(TPV_PASS_SPLIT)
  sPass.fogMs += ms_since(tFog);
#endif
  if (lineMode) {
    if (decoded.chunks != nullptr) {
      for (const auto& chunk : *decoded.chunks) {
        submit_geometry(*program, chunk.vertices.data(),
                        static_cast<uint32_t>(chunk.vertices.size() / program->layout.stride), chunk.indices.data(),
                        static_cast<uint32_t>(chunk.indices.size()));
      }
    }
    return;
  }
  const bool indexed = draw.indexCount != 0;
  const auto* indices =
      indexed ? reinterpret_cast<const uint16_t*>(frame.indices.data() + draw.idxRange.offset) : nullptr;
  submit_geometry(*program, decoded.vertices, draw.vtxCount, indices, draw.indexCount);
}

void draw_op(const Frame& frame, const gx::DrawData& draw, uint32_t drawIndex) {
  const gx::PipelineConfig* config = find_pipeline(draw.pipeline);
  if (config == nullptr || draw.vtxCount == 0) {
    return;
  }
  const bool lineMode = config->shaderConfig.lineMode != 0;
  if (!lineMode && draw.indexCount == 0 && draw.vtxCount < 3) {
    return;
  }
  // The layout depends only on the shader config, so both passes (and either program variant) share it.
  const gx::vita::DecodedLayout layout = gx::vita::decoded_layout(config->shaderConfig);
  // The decode itself ran on the worker while this thread was doing the previous draws' GL work
  // (HANDOVER 7.22). What is left here is the rendezvous: normally zero, because the worker is ahead.
  // If the worker never started, or this draw was not posted to it, decode happens inline exactly as
  // before -- the overlapped and the fallback path differ only in WHERE the bytes came from.
  const uint32_t ordinal =
      drawIndex < sOrdinalOfDraw.size() ? sOrdinalOfDraw[drawIndex] : UINT32_MAX;
  DecodedView decoded;
  const DecodeSlot* slot = decode_wait(ordinal, drawIndex);
  if (slot != nullptr) {
    if (sVerifyFrames > 0) {
      decode_verify(frame, draw, *config, lineMode, *slot);
    }
    decoded.vertices = slot->decoded.data();
    decoded.chunks = &slot->chunks;
  } else {
    const auto decodeStart = std::chrono::steady_clock::now();
    ++sWorker.inlineDecodes;
    if (lineMode) {
      gx::vita::expand_lines(*config, draw, frame, layout, sScratch.chunks);
      decoded.chunks = &sScratch.chunks;
    } else {
      sScratch.decoded.resize(static_cast<size_t>(draw.vtxCount) * layout.stride);
      gx::vita::decode_draw(*config, draw, frame, layout, sScratch.decoded);
      decoded.vertices = sScratch.decoded.data();
    }
    sPhase.decodeMs += ms_since(decodeStart);
  }
  // A line draw whose expansion produced no chunk has nothing to submit; release the slot either way.
  if (lineMode && (decoded.chunks == nullptr || decoded.chunks->empty())) {
    if (slot != nullptr) {
      decode_release(ordinal);
    }
    return;
  }
  ++sPhase.draws;
  // `pass` = everything draw_op spends that is not decode and not submit_geometry: program/uniform binds,
  // texture binds, the fog-range LUT upload. Taken as the enclosing span minus what the two inner timers
  // already booked, so the four phases stay disjoint and sum to the frame.
  const auto passStart = std::chrono::steady_clock::now();
  const double submitBefore = sPhase.submitMs;
  struct PassTimer {
    std::chrono::steady_clock::time_point start;
    double submitBefore;
    ~PassTimer() {
      const double whole = ms_since(start);
      const double submitInside = sPhase.submitMs - submitBefore;
      const double pass = whole - submitInside;
      sPhase.passMs += pass > 0 ? pass : 0.0;
    }
  } passTimer{passStart, submitBefore};
  if (dst_alpha_active(*config) && needs_source_alpha(*config)) {
    // No dual-source blending: write the constant destination alpha first (no color, no depth), then
    // draw the colors with alpha masked out.
    gx::PipelineConfig alphaPass = *config;
    alphaPass.colorUpdate = false;
    alphaPass.depthUpdate = false;
    draw_pass(frame, draw, alphaPass, lineMode, decoded);
    gx::PipelineConfig colorPass = *config;
    colorPass.alphaUpdate = false;
    draw_pass(frame, draw, colorPass, lineMode, decoded);
  } else {
    draw_pass(frame, draw, *config, lineMode, decoded);
  }
  if (slot != nullptr) {
    // Timed, because `pass` is an enclosing span and this runs inside it (HANDOVER 7.25).
    decode_release_timed(ordinal);
  }
}

// ---- EFB copies -------------------------------------------------------------------------------

// Depth EFB copies (GX_TF_Z8 / GX_TF_Z16, used for the game's Z textures) are NOT supported: they go
// through conversion_for() == Conv::Unsupported and the destination is filled with white (= far plane).
// A CPU readback is not an option: glReadPixels cannot read depth on this stack (GLES2 core has no
// depth readback, and vitaGL's glReadPixels only handles colour formats and raises GL_INVALID_ENUM for
// GL_DEPTH_COMPONENT, leaving the buffer untouched). The route vitaGL itself offers is
// vglTexImageDepthBuffer(GL_TEXTURE_2D) (present in the installed libvitaGL.a): it copies the bound
// FBO's depth buffer into the bound texture, which a conversion shader can then turn into I8/IA8 like
// the colour copies. It needs vitaGL built with STORE_DEPTH_STENCIL=1 (not verified for the vdpm build)
// and costs speed; not done, and untestable without a device.

// TEMPORARY EFB-COPY PROBE. Appended to the build to answer one question: when the game asks for an
// EFB copy, does the destination actually receive the framebuffer, or does it stay empty? The
// all-red, no-cool-tones frame we render is consistent with copies landing nothing. Remove this
// block (the counters, the probe and the summary) once the measurement is recorded in HANDOVER.
uint32_t sResolveSeen = 0;          // resolve ops in this run
uint32_t sResolveDrawn = 0;         // copies that reached draw_quad
uint32_t sResolveNoDest = 0;        // copy requested, destination texture absent
uint32_t sResolveNoSource = 0;      // copy requested, source target had no size
std::chrono::steady_clock::time_point sWindowStart = std::chrono::steady_clock::now();
// Batching survey budget (HANDOVER 7.3c). Two gates were wrong before this one, and both produced a
// confident answer to the wrong question:
//   * `sFrameIndex == 2760` -- the driving script ends near frame 2400, so it never fired at all and the
//     run produced no measurement (an empty log that reads like "nothing to report");
//   * the first frame with >= 100 draws -- that fired at frame 240, a LOADING frame with 163 draws,
//     while the scene that matters has 420-440. A threshold low enough to always fire is low enough to
//     fire on the wrong frame.
// So: sample up to a few times, only on frames that look like a full scene, and print a line per sample.
// Draw counts are the load signal here, and they are the same numbers the `window @frame` lines report.
constexpr size_t kSurveyMinDraws = 350;
constexpr uint32_t kSurveyMaxSamples = 6;
uint32_t sBatchingSurveys = 0;

// NOTE (2026-11-02): a glReadPixels-based EFB-copy probe was tried here and REMOVED because it
// measured nothing. In this context glReadPixels returned all zeros even for the default
// framebuffer at frame 600, when the captured PNG proves the presented surface has content. Do not
// reintroduce a readback probe without first proving glReadPixels works here. The counters below
// are kept because the copy VOLUME they report is real and is the useful finding.

void resolve_op(const ResolveOp& r) {
  ++sResolveSeen;
  const auto resolveStart = std::chrono::steady_clock::now();
  struct ResolveTimer {
    std::chrono::steady_clock::time_point start;
    ~ResolveTimer() { sPhase.resolveMs += ms_since(start); }
  } resolveTimer{resolveStart};
  Target& source = *sCurrent;
  const bool fullTarget = r.rect.x <= 0 && r.rect.y <= 0 && r.rect.x + r.rect.width >= int32_t(source.width) &&
                          r.rect.y + r.rect.height >= int32_t(source.height);
  TextureRef* dst = r.texture.get();
  if (dst != nullptr && dst->glTexture != 0 && source.width != 0 && source.height != 0) {
    const Conv conv = conversion_for(r.resolveFormat);
    static bool warnedUnsupported = false;
    if (conv == Conv::Unsupported && !warnedUnsupported) {
      warnedUnsupported = true;
      Log.warn("EFB copy to depth format 0x{:X} is not supported on this backend; copies are filled with white",
               static_cast<unsigned>(r.resolveFormat));
    }
    QuadProgram& q = conv_program(conv);
    if (q.program != 0) {
      if (dst->glFbo == 0) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->glTexture, 0);
        dst->glFbo = fbo;
      } else {
        glBindFramebuffer(GL_FRAMEBUFFER, dst->glFbo);
      }
      setup_fullscreen_state(false);
      glViewport(0, 0, static_cast<GLint>(dst->size.width), static_cast<GLint>(dst->size.height));
      bind_texture(0, source.color);
      const float invW = 1.f / static_cast<float>(source.width);
      const float invH = 1.f / static_cast<float>(source.height);
      float v0 = static_cast<float>(r.rect.y) * invH;
      float vScale = static_cast<float>(r.rect.height) * invH;
      if (kFboRowsFlipped) {
        v0 = 1.f - v0;
        vScale = -vScale;
      }
      draw_quad(q, static_cast<float>(r.rect.x) * invW, v0, static_cast<float>(r.rect.width) * invW, vScale,
                !gx::efb_has_alpha(r.sourceFormat));
      ++sResolveDrawn;
      mark_texture_written(*dst); // textures derived from this copy (palette lookups) must be recomputed
      // The destination texture now holds new contents; make sure no stale binding is reused.
      for (uint32_t i = 0; i < kTextureUnits; ++i) {
        if (sCache.boundTextureValid[i] && sCache.boundTexture[i] == dst->glTexture) {
          sCache.boundTextureValid[i] = false;
        }
      }
      bind_target(*sCurrent); // back to the render target we were drawing into
    }
  } else {
    ++sResolveNoDest;
  }
  if (r.clearColor || r.clearAlpha || r.clearDepth) {
    if (fullTarget) {
      clear_current(r.clearColor, r.clearAlpha, r.clearDepth, r.clearColorValue, r.clearDepthValue, true);
    } else {
      // Only the copied rectangle is cleared (GX clears what it copied).
      const int32_t left = std::clamp<int32_t>(r.rect.x, 0, static_cast<int32_t>(source.width));
      const int32_t top = std::clamp<int32_t>(r.rect.y, 0, static_cast<int32_t>(source.height));
      const int32_t right = std::clamp<int32_t>(r.rect.x + r.rect.width, left, static_cast<int32_t>(source.width));
      const int32_t bottom = std::clamp<int32_t>(r.rect.y + r.rect.height, top, static_cast<int32_t>(source.height));
      if (right > left && bottom > top) {
        glScissor(left, top, right - left, bottom - top);
        clear_current(r.clearColor, r.clearAlpha, r.clearDepth, r.clearColorValue, r.clearDepthValue, false);
        apply_scissor(source); // back to the game's scissor rectangle
      }
    }
  }
}

} // namespace

// ---- public entry points ----------------------------------------------------------------------

// The size the game *thinks* it is drawing into: the VI render mode (608x448 for Twilight Princess).
// Every viewport and scissor the game sets is authored in these units, so this is the divisor that
// turns them into EFB pixels.
Vec2<uint32_t> main_target_size() noexcept { return aurora::vi::configured_fb_size(); }

// The EFB actually allocated, in pixels: main_target_size() reduced by kEfbScalePercent. This is what
// the GL replay binds and what gfx::get_render_target_size() must report for the main target, or the
// logical->EFB mapping in gx::map_logical_viewport() silently computes a scale of 1.0.
Vec2<uint32_t> efb_target_size() noexcept {
  const auto size = aurora::vi::configured_fb_size();
  return {std::max<uint32_t>(size.x * kEfbScalePercent / 100, 1),
          std::max<uint32_t>(size.y * kEfbScalePercent / 100, 1)};
}

void set_shader_dump_dir(const char* dir) noexcept {
  sShaderDumpDir = dir != nullptr ? dir : "";
  sShaderDumps = 0;
}

void set_present_size(uint32_t width, uint32_t height) noexcept {
  sPresentWidth = std::max<uint32_t>(width, 1);
  sPresentHeight = std::max<uint32_t>(height, 1);
}

void replay(Frame& frame) {
  ++sFrameIndex;
  if (sVerifyFrames > 0) {
    --sVerifyFrames;
    if (sVerifyFrames == 0) {
      Log.warn("decode worker A/B complete: {} results compared, {} differed ({} line draws). "
               "A non-zero difference count means the overlap is NOT safe to ship.",
               sVerifyChecks, sVerifyDiffs, sVerifySkipped);
    }
  }
  // Start a fresh per-frame phase record; `other` is filled at the end as the remainder.
  sPhase = PhaseTimes{};
  sLastSetupMs = sOtherSetupMs;
  sLastClearMs = sOtherClearMs;
  sLastTailMs = sOtherTailMs;
  sOtherSetupMs = sOtherClearMs = sOtherTailMs = 0;
  const auto frameStart = std::chrono::steady_clock::now();
  const auto setupStart = std::chrono::steady_clock::now();
  init_gl();
  flush_texture_deletions();
  ensure_efb();
  invalidate_cache();
  sOtherSetupMs += ms_since(setupStart);
  // Hand the frame's whole draw list to the decode worker BEFORE the op loop starts, so 36.9 % of the
  // frame decodes while this thread does the clear, the state changes and the GL calls. See the block
  // above decode_worker_main() for why this is safe and what it is worth.
  decode_publish(frame);
  // Breadcrumbs for a first run: the first frames, then a heartbeat. If the process dies, the last line in the
  // log (flushed per Info line) says which frame it was working on and how many programs it had compiled.
  // TEMPORARY TIMING PROBE (item 4). An emulator cannot give a console frame rate, but it can give the
  // SHAPE of the cost. Three deliberate design choices, each from a measurement that was wrong without
  // it:
  //  * a DELTA over a fixed window, never a cumulative average, or the compile-dominated startup
  //    swamps everything;
  //  * 120-frame windows, because shader compilation costs 200-700 ms per program and the first ~70
  //    programs are built lazily, so a 300-frame window did not even fit in a 60 s run;
  //  * `Log.warn` rather than `Log.info`, because a plain `info` into a pipe is block-buffered and the
  //    test harness SIGKILLs the process, which truncates lines mid-write and looks like a hang.
  const auto windowNow = std::chrono::steady_clock::now();
  // BATCHING SURVEY (HANDOVER 7.3c). Kept as evidence, not as a plan: it measured that batching is NOT
  // worth doing here. A 442-draw play-scene frame has 17 pipeline groups and 211 state-identical
  // (program+texture) runs, which reads like 2x, but only 65 adjacent pairs also share identical uniform
  // CONTENT, so a legal order-preserving batcher reaches 377 calls -- 1.17x, not 2.6x. Uniforms are per
  // draw (`uniformRange` plus `immediateData`); `currentPnMtx` is NOT an obstacle because it is baked
  // per-vertex in vita_vertex.cpp, but the uniform BYTES still differ nearly always. Compare content,
  // never offsets: the frame arena reuses storage, so 0 of 441 pairs share an offset while 69 share a
  // blob. Delete this block, do not extend it, unless the merge criterion changes.
  if (sFrameIndex % 120 == 0) {
    const double windowMs = std::chrono::duration<double, std::milli>(windowNow - sWindowStart).count();
    sWindowStart = windowNow;
    Log.warn("window @frame {}: 120 frames, {:.0f} ms wall = {:.2f} ms/frame ({:.1f} fps equivalent); "
             "{} draws this frame, {} programs total, {} resolve ops seen",
             sFrameIndex, windowMs, windowMs / 120.0, 120000.0 / (windowMs > 0 ? windowMs : 1),
             frame.draws.size(), sPrograms.size(), sResolveSeen);
    // WHERE OUR FRAME GOES (HANDOVER 7.21). Reported as a share of the summed phases so it stays
    // meaningful in an emulator whose absolute times are not a console's: read the RATIOS, not the ms.
    {
      const double total = sPhaseWindow.decodeMs + sPhaseWindow.submitMs + sPhaseWindow.passMs +
                           sPhaseWindow.resolveMs + sPhaseWindow.otherMs;
      auto pct = [total](double v) { return total > 0 ? 100.0 * v / total : 0.0; };
      Log.warn("  phases: decode {:.1f}% | submit {:.1f}% | pass {:.1f}% | resolve {:.1f}% | "
               "other {:.1f}%; {} draws",
               pct(sPhaseWindow.decodeMs), pct(sPhaseWindow.submitMs), pct(sPhaseWindow.passMs),
               pct(sPhaseWindow.resolveMs), pct(sPhaseWindow.otherMs), sPhaseWindow.draws);
      // `other` broken down, as a share of the whole frame. If `tail` dominates, `other` is per-op work
      // inside the op loop; if `setup` or `clear` dominates, it is per-frame overhead.
      // `other` is broken down per frame; the values shown are this window's last frame, which is
      // representative because a scene's composition is stable (measured: see HANDOVER 7.21).
      Log.warn("    other split of frame: setup {:.1f}% | clear {:.1f}% | tail {:.1f}% (survey excluded)",
               pct(sLastSetupMs), pct(sLastClearMs), pct(sLastTailMs));
#if defined(TPV_PASS_SPLIT)
      // `pass` attributed. Shares are of `pass` itself, so they sum to 100 % and each name is the call it
      // wraps in draw_pass(). The bind counts are what actually reached gl*, i.e. what the caches did NOT
      // suppress: if those are near zero while the milliseconds are not, the cost is the checking.
      {
        const double passTotal = sPassWindow.programMs + sPassWindow.stateMs + sPassWindow.uniformMs +
                                 sPassWindow.textureMs + sPassWindow.fogMs;
        auto ppct = [passTotal](double v) { return passTotal > 0 ? 100.0 * v / passTotal : 0.0; };
        Log.warn("    pass split: program {:.1f}% | state {:.1f}% | uniform {:.1f}% | texture {:.1f}% | "
                 "fog {:.1f}% of pass ({} passes, {:.2f} ms of frame)",
                 ppct(sPassWindow.programMs), ppct(sPassWindow.stateMs), ppct(sPassWindow.uniformMs),
                 ppct(sPassWindow.textureMs), ppct(sPassWindow.fogMs), sPassWindow.passes, passTotal);
        Log.warn("    pass binds reaching gl*: {} texture, {} fog LUT, over {} passes ({:.2f} and {:.2f} "
                 "per pass)",
                 sTextureBindsWindow, sFogUploadsWindow, sPassWindow.passes,
                 sPassWindow.passes ? static_cast<double>(sTextureBindsWindow) / sPassWindow.passes : 0.0,
                 sPassWindow.passes ? static_cast<double>(sFogUploadsWindow) / sPassWindow.passes : 0.0);
      }
#endif
      // The decode worker's own accounting, as a share of the same frame total. `decode` above is now
      // only the part that could NOT be overlapped (the rendezvous plus any inline fallback), so the
      // reading that decides whether this worked is the PAIR: if `decode` is still ~37 % the work did
      // not move, and if it collapsed while `off-thread` is ~37 % then it did. `inline` must be 0 on
      // any window the worker handled; anything else means the fallback ran and the change is inactive.
      // Read and reset under the lock: the worker may be decoding the current frame right now, which is
      // precisely the case this window boundary is measuring.
      double offthreadMs;
      uint32_t offthreadDraws;
      {
        std::lock_guard<std::mutex> lock(sWorker.mu);
        offthreadMs = sWorker.decodeMs;
        offthreadDraws = sWorker.decodedDraws;
        sWorker.decodeMs = 0;
        sWorker.decodedDraws = 0;
      }
      Log.warn("    decode worker: {:.1f}% of frame decoded off-thread ({} draws, {:.2f} ms), "
               "rendezvous {:.1f}% ({:.2f} ms), {} inline fallbacks, {} slots, {} threads",
               pct(offthreadMs), offthreadDraws, offthreadMs, pct(sPhaseWindow.decodeMs),
               sPhaseWindow.decodeMs, sWorker.inlineDecodes, kDecodeSlots, sWorker.running ? 2 : 1);
      Log.warn("    decode worker waits: {} calls, {} blocked ({:.2f}% of draws)", sWorker.waitCalls,
               sWorker.waitBlocks,
               sWorker.waitCalls ? 100.0 * static_cast<double>(sWorker.waitBlocks) / sWorker.waitCalls : 0.0);
      // Attribution of the `pass` inflation the worker causes (7.25). `pass` is an enclosing span, so
      // whatever this thread blocks on inside it is booked there; these two numbers say whether the cause
      // is the hand-off itself or something no timer here can see.
      Log.warn("    decode worker hand-off cost: release {:.1f}% of frame ({} calls), booked inside `pass`",
               pct(sReleaseMs), sReleaseCalls);
      Log.warn("    decode worker slot handshake: {} releases, {} signalled the worker ({:.2f}%)",
               sWorker.releaseCalls, sWorker.slotNotifies,
               sWorker.releaseCalls ? 100.0 * static_cast<double>(sWorker.slotNotifies) / sWorker.releaseCalls
                                    : 0.0);
      sWorker.releaseCalls = 0;
      sWorker.slotNotifies = 0;
      sReleaseMs = 0;
      sReleaseCalls = 0;
      sWorker.waitCalls = 0;
      sWorker.waitBlocks = 0;
      // One line, not two: this was printed twice, once guarded and once not, so a build with verification
      // OFF still reported a meaningless "0 compared, 0 differed".
      if (sVerifyChecks > 0) {
        Log.warn("    decode worker A/B: {} results compared against an inline decode, {} differed ({} line draws)",
                 sVerifyChecks, sVerifyDiffs, sVerifySkipped);
      }
      sWorker.inlineDecodes = 0;
      sPhaseWindow = PhaseTimes{};
    }
    gx::vita::vertex_cache_report("window");
    const auto surveyStart = std::chrono::steady_clock::now();
    // ONE-SHOT BATCHING SURVEY (HANDOVER 7.3c). Before anyone spends a session on draw batching, answer
    // the question that decides whether it is possible at all: how many of this frame's draws share a
    // pipeline AND a blend setup, i.e. could legally be merged into one longer draw if their vertex data
    // were contiguous? Latched, so it costs one line for the whole run, and triggered by a draw-count
    // threshold rather than a fixed frame number -- a fixed frame number silently produced nothing when
    // the driving script ended before it (see the latch comment near sWindowStart).
    if (sBatchingSurveys < kSurveyMaxSamples && frame.draws.size() >= kSurveyMinDraws) {
      ++sBatchingSurveys;
      // The MERGE CRITERION is not "same pipeline". Two draws can only become one glDraw* if they share
      // the program AND every sampler binding AND one contiguous vertex buffer, because the decoded
      // vertices are uploaded through a single glVertexAttribPointer set. Reporting only the pipeline
      // count would overstate what is possible: it dropped textures, which vary per draw.
      //
      // Three numbers, in increasing strictness:
      //   pipelineGroups  - same PipelineConfig only (the optimistic ceiling, NOT achievable)
      //   stateGroups     - same PipelineConfig AND same texture bind group (the real merge criterion)
      //   runs            - adjacent-in-order groups: the only ones mergeable WITHOUT reordering draws,
      //                     which is what an order-preserving batcher could actually do.
      auto key_of = [](const gx::DrawData& d, const gx::PipelineConfig* c) {
        uint64_t h = d.pipeline;
        h = h * 1099511628211ull ^ d.bindGroups.textureBindGroup;
        h = h * 1099511628211ull ^ static_cast<uint64_t>(std::hash<std::string_view>{}(
            std::string_view(reinterpret_cast<const char*>(c), sizeof(*c))));
        return h;
      };
      std::unordered_map<uint64_t, uint32_t> pipelineGroups;
      std::unordered_map<uint64_t, uint32_t> stateGroups;
      uint32_t runs = 0;
      uint64_t prevKey = ~0ull;
      // The strict criterion: identical uniform blob AND identical immediate data (currentPnMtx plus the
      // per-attribute arrayStart values). Two adjacent draws that also match these are genuinely one draw.
      uint32_t strictRuns = 0;
      uint64_t prevStrict = ~0ull;
      uint32_t uniformShared = 0; // adjacent pairs sharing the same uniformRange offset
      for (const auto& d : frame.draws) {
        const gx::PipelineConfig* c = find_pipeline(d.pipeline);
        if (c == nullptr) {
          continue;
        }
        ++pipelineGroups[static_cast<uint64_t>(d.pipeline)];
        const uint64_t key = key_of(d, c);
        ++stateGroups[key];
        if (key != prevKey) {
          ++runs; // first draw of a new adjacent group
          prevKey = key;
        }
        uint64_t strict = key;
        strict = strict * 1099511628211ull ^ d.uniformRange.offset;
        strict = strict * 1099511628211ull ^ d.uniformRange.size;
        strict = strict * 1099511628211ull ^
                 static_cast<uint64_t>(std::hash<std::string_view>{}(std::string_view(
                     reinterpret_cast<const char*>(&d.immediateData), sizeof(d.immediateData))));
        if (strict != prevStrict) {
          ++strictRuns;
          prevStrict = strict;
        }
      }
      // How much do adjacent draws actually share? Same OFFSET is the wrong test: the frame arena reuses
      // storage, so two draws with different offsets can hold identical bytes. The question that decides
      // batching is whether adjacent draws in the same state group carry the SAME UNIFORM CONTENT, because
      // a merged draw can only have one uniform set. `currentPnMtx` is NOT an obstacle: it is baked into
      // the decoded vertices (vita_vertex.cpp:427 sets `p.constant`), and the matrices it indexes live in
      // the uniform array, so two draws differing only in PN index ARE mergeable.
      auto blob_of = [&frame](const gx::DrawData& d) {
        const size_t off = d.uniformRange.offset;
        const size_t len = d.uniformRange.size;
        if (off > frame.uniforms.size() || len > frame.uniforms.size() - off) {
          return std::string_view{};
        }
        return std::string_view(reinterpret_cast<const char*>(frame.uniforms.data() + off), len);
      };
      uint32_t sameBytesAdjacent = 0;
      uint32_t mergeablePairs = 0;
      for (size_t i = 1; i < frame.draws.size(); ++i) {
        const gx::DrawData& a = frame.draws[i - 1];
        const gx::DrawData& b = frame.draws[i];
        const gx::PipelineConfig* ca = find_pipeline(a.pipeline);
        const gx::PipelineConfig* cb = find_pipeline(b.pipeline);
        if (ca == nullptr || cb == nullptr) {
          continue;
        }
        if (a.uniformRange.offset == b.uniformRange.offset) {
          ++uniformShared;
        }
        const auto ba = blob_of(a);
        const auto bb = blob_of(b);
        if (!ba.empty() && ba == bb) {
          ++sameBytesAdjacent;
          // Mergeable = same state AND same uniform content AND same fp state.
          if (key_of(a, ca) == key_of(b, cb)) {
            ++mergeablePairs;
          }
        }
      }
      // The number that decides whether a batcher pays is not "how many groups" but "how many CALLS are
      // saved": a run of 2 saves 1 call, a run of 40 saves 39. Compute the achievable call count by
      // walking the draws again and merging a run only while state+bytes stay identical, then report the
      // saving and the longest run, which is what a real batcher would be worth.
      uint32_t callsIfBatched = 0;
      uint32_t longestRun = 0;
      uint32_t runLen = 0;
      bool havePrev = false;
      uint64_t prevMerge = ~0ull;
      std::string_view prevBlob{};
      for (const auto& d : frame.draws) {
        const gx::PipelineConfig* c = find_pipeline(d.pipeline);
        if (c == nullptr) {
          continue;
        }
        const uint64_t k = key_of(d, c);
        const auto blob = blob_of(d);
        const bool same = havePrev && k == prevMerge && !blob.empty() && blob == prevBlob;
        if (same) {
          ++runLen;
        } else {
          ++callsIfBatched;
          if (runLen > longestRun) {
            longestRun = runLen;
          }
          runLen = 1;
          havePrev = true;
          prevMerge = k;
          prevBlob = blob;
        }
      }
      if (runLen > longestRun) {
        longestRun = runLen;
      }
      const uint32_t pairs = frame.draws.size() > 1 ? static_cast<uint32_t>(frame.draws.size() - 1) : 0;
      Log.warn("batching survey @frame {}: {} draws -> {} pipelineGroups, {} stateGroups, {} adjacent "
               "runs (program+textures), {} STRICT runs (offset+immediate); of {} adjacent pairs: {} "
               "share a uniform OFFSET, {} share uniform BYTES, {} are fully mergeable (same state+bytes); "
               "batched call count {} (longest run {})",
               sFrameIndex, frame.draws.size(), pipelineGroups.size(), stateGroups.size(), runs, strictRuns,
               pairs, uniformShared, sameBytesAdjacent, mergeablePairs, callsIfBatched, longestRun);
    }
  }
  if (sFrameIndex <= 5) {
    Log.info("frame {}: {} draws, {} programs compiled so far; resolve {} seen / {} drawn / {} no-destination",
             sFrameIndex, frame.draws.size(), sPrograms.size(), sResolveSeen, sResolveDrawn, sResolveNoDest);
  }

  const auto clearStart = std::chrono::steady_clock::now();
  Target* efb = &sEfb;
  bind_target(*efb);
  clear_current(true, true, true, frame.clearColor, frame.clearDepth, true);
  sOtherClearMs += ms_since(clearStart);

  Target* suspended = nullptr; // the EFB while an offscreen target is active
  for (const Op& op : frame.ops) {
    switch (op.kind) {
    case Op::Kind::Draw:
      draw_op(frame, frame.draws[op.index], op.index);
      break;
    case Op::Kind::Viewport:
      sCurrent->viewport = op.viewport;
      apply_viewport(*sCurrent);
      break;
    case Op::Kind::Scissor:
      sCurrent->scissor = op.scissor;
      apply_scissor(*sCurrent);
      break;
    case Op::Kind::Resolve:
      resolve_op(frame.resolves[op.index]);
      break;
    case Op::Kind::BeginOffscreen: {
      if (suspended == nullptr) {
        suspended = sCurrent;
      }
      Target* target = offscreen_target(op.width, op.height);
      target->viewport = {0.f, 0.f, static_cast<float>(op.width), static_cast<float>(op.height), 0.f, 1.f};
      target->scissor = {0, 0, static_cast<int32_t>(op.width), static_cast<int32_t>(op.height)};
      bind_target(*target);
      clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 0.f}, 1.f, true);
      break;
    }
    case Op::Kind::EndOffscreen:
      if (suspended != nullptr) {
        bind_target(*suspended);
        suspended = nullptr;
      }
      break;
    }
  }
  if (suspended != nullptr) {
    Log.warn("frame ended inside an offscreen pass");
    bind_target(*suspended);
  }

  // Every posted draw has been consumed by the loop above, so this only waits when the worker could not
  // keep up. It also guarantees the worker is not still reading this frame when the next publish swaps
  // `jobs` -- which is the one ordering hazard the ring does not cover on its own.
  decode_drain();

  // Close the frame's phase record. `other` is everything replay() spent outside the four draw phases;
  // setup and clear are timed directly, and the tail (per-op viewport/scissor/offscreen binds and any
  // remaining bookkeeping) is what is left of `other` after those two.
  const double frameMs = ms_since(frameStart);
  {
    const double drawPhases = sPhase.decodeMs + sPhase.submitMs + sPhase.passMs + sPhase.resolveMs;
    sPhase.otherMs = frameMs > drawPhases ? frameMs - drawPhases : 0.0;
    const double tail = sPhase.otherMs - sOtherSetupMs - sOtherClearMs;
    sOtherTailMs += tail > 0 ? tail : 0.0;
  }
#if defined(TPV_PASS_SPLIT)
  // Snapshot and accumulate here, at end of frame, for the same reason as sLastSetupMs: the report runs
  // after the next frame's reset, so reading the per-frame struct there prints zeros (HANDOVER trap 32).
  sLastPass = sPass;
  sPassWindow.programMs += sPass.programMs;
  sPassWindow.stateMs += sPass.stateMs;
  sPassWindow.uniformMs += sPass.uniformMs;
  sPassWindow.textureMs += sPass.textureMs;
  sPassWindow.fogMs += sPass.fogMs;
  sPassWindow.passes += sPass.passes;
  sTextureBindsWindow += sTextureBindCount;
  sFogUploadsWindow += sFogUploadCount;
  sPass = PassSplit{};
  sTextureBindCount = 0;
  sFogUploadCount = 0;
#endif
  sPhaseWindow.decodeMs += sPhase.decodeMs;
  sPhaseWindow.submitMs += sPhase.submitMs;
  sPhaseWindow.passMs += sPhase.passMs;
  sPhaseWindow.resolveMs += sPhase.resolveMs;
  sPhaseWindow.otherMs += sPhase.otherMs;
  sPhaseWindow.draws += sPhase.draws;
}

void present() {
  if (!sGlReady || sEfb.fbo == 0) {
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  sCurrent = nullptr;
  setup_fullscreen_state(false);
  glViewport(0, 0, static_cast<GLint>(sPresentWidth), static_cast<GLint>(sPresentHeight));
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);

  const double scale = std::min(static_cast<double>(sPresentWidth) / sEfb.width,
                                static_cast<double>(sPresentHeight) / sEfb.height);
  const auto width = static_cast<GLint>(std::lround(sEfb.width * scale));
  const auto height = static_cast<GLint>(std::lround(sEfb.height * scale));
  glViewport((static_cast<GLint>(sPresentWidth) - width) / 2, (static_cast<GLint>(sPresentHeight) - height) / 2, width,
             height);
  if (sPresent.program == 0) {
    sPresent.program = link_program(QuadVertexSource,
                                    "precision highp float;\nvarying vec2 vUv;\nuniform sampler2D uSrc;\nuniform vec4 "
                                    "uFlags;\nvoid main() { gl_FragColor = vec4(texture2D(uSrc, vUv).rgb, 1.0); }\n",
                                    QuadAttribs, 2, "present");
    finish_quad_program(sPresent);
  }
  if (sPresent.program != 0) {
    bind_texture(0, sEfb.color);
    // Default framebuffer: window y = 0 is the bottom of the screen, EFB row 0 is the top of the image.
    draw_quad(sPresent, 0.f, kFboRowsFlipped ? 0.f : 1.f, 1.f, kFboRowsFlipped ? 1.f : -1.f, false);
  }
}

bool read_efb_rgba(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height) {
  if (!sGlReady || sEfb.fbo == 0) {
    return false;
  }
  width = sEfb.width;
  height = sEfb.height;
  out.resize(static_cast<size_t>(width) * height * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, sEfb.fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
  sCurrent = nullptr;
  return true;
}

void shutdown_gl() {
  if (!sGlReady) {
    return;
  }
  for (auto& [hash, program] : sPrograms) {
    if (program->program != 0) {
      glDeleteProgram(program->program);
    }
  }
  sPrograms.clear();
  sLightLocs.clear();
  for (auto& q : sConv) {
    if (q.program != 0) {
      glDeleteProgram(q.program);
    }
    q = {};
  }
  if (sPresent.program != 0) {
    glDeleteProgram(sPresent.program);
  }
  sPresent = {};
  for (auto& [key, target] : sOffscreen) {
    destroy_target(*target);
  }
  sOffscreen.clear();
  destroy_target(sEfb);
  if (sFallbackTexture != 0) {
    glDeleteTextures(1, &sFallbackTexture);
    sFallbackTexture = 0;
  }
  if (sFogRangeTexture != 0) {
    glDeleteTextures(1, &sFogRangeTexture);
    sFogRangeTexture = 0;
    sFogRangeRgba.clear();
  }
  // Release every TextureRef the GX layer still holds (bound objects, TLUT/dynamic-palette and copy
  // caches), then delete their GL objects while the context is still current. Without this the
  // caches outlive the context and their destructors run during static teardown.
  gx::shutdown();
  shutdown_textures();
  sCurrent = nullptr;
  sGlReady = false;
}
} // namespace aurora::gfx::vita
