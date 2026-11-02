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
#include <cstdio>
#include <cstring>
#include <memory>
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

void apply_viewport(const Target& t) {
  const Viewport& v = t.viewport;
  const auto round = [](float x) { return static_cast<GLint>(std::lround(x)); };
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
constexpr uint32_t kEfbScalePercent = 75;

void ensure_efb() {
  const auto size = main_target_size();
  const uint32_t width = std::max<uint32_t>(size.x * kEfbScalePercent / 100, 1);
  const uint32_t height = std::max<uint32_t>(size.y * kEfbScalePercent / 100, 1);
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

void submit_geometry(const Program& p, const uint8_t* vertices, uint32_t vertexCount, const uint16_t* indices,
                     uint32_t indexCount) {
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
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * 4);
  for (uint32_t x = 0; x < width; ++x) {
    float factor;
    std::memcpy(&factor, frame.storage.data() + byteBase + static_cast<size_t>(x) * sizeof(float), sizeof(float));
    uint8_t* texel = &rgba[static_cast<size_t>(x) * 4];
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
  if (sFogRangeRgba != rgba) {
    glBindTexture(GL_TEXTURE_2D, sFogRangeTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    sFogRangeRgba = std::move(rgba);
  }
  bind_texture(kFogRangeUnit, sFogRangeTexture);
  if (p.locFogRangeSize >= 0) {
    glUniform2f(p.locFogRangeSize, 1.f / static_cast<float>(width), 0.f);
  }
}

// One program pass of a draw; `decoded` / `chunks` are already filled for this draw.
void draw_pass(const Frame& frame, const gx::DrawData& draw, const gx::PipelineConfig& config, bool lineMode) {
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
  apply_pass_state(to_pass_state(config, replace));
  upload_uniforms(*program, frame, draw);
  bind_textures(*program, draw);
  bind_fog_range(frame, draw, *program);
  if (lineMode) {
    for (const auto& chunk : sScratch.chunks) {
      submit_geometry(*program, chunk.vertices.data(),
                      static_cast<uint32_t>(chunk.vertices.size() / program->layout.stride), chunk.indices.data(),
                      static_cast<uint32_t>(chunk.indices.size()));
    }
    return;
  }
  const bool indexed = draw.indexCount != 0;
  const auto* indices =
      indexed ? reinterpret_cast<const uint16_t*>(frame.indices.data() + draw.idxRange.offset) : nullptr;
  submit_geometry(*program, sScratch.decoded.data(), draw.vtxCount, indices, draw.indexCount);
}

void draw_op(const Frame& frame, const gx::DrawData& draw) {
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
  if (lineMode) {
    gx::vita::expand_lines(*config, draw, frame, layout, sScratch.chunks);
    if (sScratch.chunks.empty()) {
      return;
    }
  } else {
    sScratch.decoded.resize(static_cast<size_t>(draw.vtxCount) * layout.stride);
    gx::vita::decode_draw(*config, draw, frame, layout, sScratch.decoded);
  }
  if (dst_alpha_active(*config) && needs_source_alpha(*config)) {
    // No dual-source blending: write the constant destination alpha first (no color, no depth), then
    // draw the colors with alpha masked out.
    gx::PipelineConfig alphaPass = *config;
    alphaPass.colorUpdate = false;
    alphaPass.depthUpdate = false;
    draw_pass(frame, draw, alphaPass, lineMode);
    gx::PipelineConfig colorPass = *config;
    colorPass.alphaUpdate = false;
    draw_pass(frame, draw, colorPass, lineMode);
  } else {
    draw_pass(frame, draw, *config, lineMode);
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

// NOTE (2026-11-02): a glReadPixels-based EFB-copy probe was tried here and REMOVED because it
// measured nothing. In this context glReadPixels returned all zeros even for the default
// framebuffer at frame 600, when the captured PNG proves the presented surface has content. Do not
// reintroduce a readback probe without first proving glReadPixels works here. The counters below
// are kept because the copy VOLUME they report is real and is the useful finding.

void resolve_op(const ResolveOp& r) {
  ++sResolveSeen;
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

Vec2<uint32_t> main_target_size() noexcept { return aurora::vi::configured_fb_size(); }

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
  init_gl();
  flush_texture_deletions();
  ensure_efb();
  invalidate_cache();
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
  if (sFrameIndex % 120 == 0) {
    const double windowMs = std::chrono::duration<double, std::milli>(windowNow - sWindowStart).count();
    sWindowStart = windowNow;
    Log.warn("window @frame {}: 120 frames, {:.0f} ms wall = {:.2f} ms/frame ({:.1f} fps equivalent); "
             "{} draws this frame, {} programs total, {} resolve ops seen",
             sFrameIndex, windowMs, windowMs / 120.0, 120000.0 / (windowMs > 0 ? windowMs : 1),
             frame.draws.size(), sPrograms.size(), sResolveSeen);
    gx::vita::vertex_cache_report("window");
  }
  if (sFrameIndex <= 5) {
    Log.info("frame {}: {} draws, {} programs compiled so far; resolve {} seen / {} drawn / {} no-destination",
             sFrameIndex, frame.draws.size(), sPrograms.size(), sResolveSeen, sResolveDrawn, sResolveNoDest);
  }

  Target* efb = &sEfb;
  bind_target(*efb);
  clear_current(true, true, true, frame.clearColor, frame.clearDepth, true);

  Target* suspended = nullptr; // the EFB while an offscreen target is active
  for (const Op& op : frame.ops) {
    switch (op.kind) {
    case Op::Kind::Draw:
      draw_op(frame, frame.draws[op.index]);
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
