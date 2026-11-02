// Vita backend: GL texture objects. See vita_texture.hpp for the contract with the replay.
#include "vita_texture.hpp"

#include "vita_gl.hpp"
#include "vita_texture_convert.hpp"

#include "../logging.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace aurora::gfx {
namespace {
constexpr Module Log{"aurora::gfx::texture"};

// vitaGL's maximum texture dimension.
constexpr uint32_t MaxTextureDim = 4096;
// Upstream's hand-authored-mip heuristic only feeds anisotropic filtering, which GLES2/vitaGL does not
// use here, so it is off (it costs a box-filter pass over every mip chain at load time).
constexpr bool DetectArbitraryMips = false;
// Decode scratch above this many texels is released after use instead of being kept for reuse.
constexpr size_t MaxRetainedScratchTexels = 4u * 1024 * 1024;

constexpr bool is_pow2(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }
uint64_t chain_bytes(uint32_t w, uint32_t h, uint32_t levels) noexcept {
  return static_cast<uint64_t>(rgba8_mip_texels(w, h, levels)) * 4;
}

// ---- deferred deletion -------------------------------------------------------------------------
struct PendingDelete {
  GLuint texture;
  GLuint fbo;
};
// Deliberately leaked: a TextureRef held by another static can be destroyed after this file's
// statics at process exit, and its destructor appends here.
std::mutex& delete_mutex() noexcept {
  static auto* mutex = new std::mutex;
  return *mutex;
}
std::vector<PendingDelete>& pending_deletes() noexcept {
  static auto* pending = new std::vector<PendingDelete>;
  return *pending;
}
std::atomic<uint64_t> sLiveBytes{0};

// ---- shared state (GL thread) ------------------------------------------------------------------
std::vector<uint32_t> sScratch;
TextureHandle sEmptyTexture;

struct PaletteProgram {
  GLuint program = 0;
  GLint uSrc = -1;
  GLint uTlut = -1;
  GLint uParams = -1;
  GLuint fbo = 0;
  bool failed = false;
} sPalette;

void trim_scratch() noexcept {
  if (sScratch.capacity() > MaxRetainedScratchTexels) {
    std::vector<uint32_t>().swap(sScratch);
  }
}

void clear_gl_errors() noexcept {
  while (glGetError() != GL_NO_ERROR) {
  }
}

// vitaGL implements glPixelStorei for GL_UNPACK_ROW_LENGTH ONLY. Every other pname (including
// GL_UNPACK_ALIGNMENT and GL_PACK_ALIGNMENT) makes it set vgl_error = GL_INVALID_ENUM, which is a
// sticky error: the call does nothing useful and poisons the next glGetError(). Because all our
// uploads are RGBA8, rows are always 4-byte aligned and the alignment calls are unnecessary on
// Vita; on the host (Mesa) they are real and kept. Confirm from the library itself:
//   arm-vita-eabi-ar x $VITASDK/arm-vita-eabi/lib/libvitaGL.a textures.o
//   arm-vita-eabi-objdump -d textures.o | sed -n '/<glPixelStorei>:/,/^$/p'
//   -> cmp r0, #0xCF2 ; strne r2(=0x500) -> [vgl_error]
#if defined(__vita__)
constexpr bool PixelStoreAlignmentSupported = false;
#else
constexpr bool PixelStoreAlignmentSupported = true;
#endif
void set_unpack_alignment(int alignment) noexcept {
  if (PixelStoreAlignmentSupported) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
  }
}
const char* gl_error_name(GLenum e) noexcept {
  switch (e) {
  case GL_NO_ERROR:
    return "GL_NO_ERROR";
  case GL_INVALID_ENUM:
    return "GL_INVALID_ENUM";
  case GL_INVALID_VALUE:
    return "GL_INVALID_VALUE";
  case GL_INVALID_OPERATION:
    return "GL_INVALID_OPERATION";
#ifdef GL_INVALID_FRAMEBUFFER_OPERATION
  // VitaSDK's GLES2/gl2.h does not define this one.
  case GL_INVALID_FRAMEBUFFER_OPERATION:
    return "GL_INVALID_FRAMEBUFFER_OPERATION";
#endif
  case GL_OUT_OF_MEMORY:
    return "GL_OUT_OF_MEMORY";
  default:
    return "GL_UNKNOWN";
  }
}

// Drains the GL error queue and reports each distinct (label, code, size) once, so a broken upload
// path produces a short actionable list instead of tens of thousands of identical lines. The code is
// named rather than discarded: that distinction is what identified the vitaGL glPixelStorei defect.
void report_gl_errors(const char* label, uint32_t w, uint32_t h, uint32_t levels, bool hasPixels) noexcept {
  struct Seen {
    const char* label;
    GLenum code;
    uint32_t w, h, levels;
    bool hasPixels;
    bool operator==(const Seen& o) const noexcept {
      return label == o.label && code == o.code && w == o.w && h == o.h && levels == o.levels &&
             hasPixels == o.hasPixels;
    }
  };
  static std::vector<Seen> reported;
  static size_t failures = 0;

  bool any = false;
  for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
    any = true;
    const Seen seen{label, e, w, h, levels, hasPixels};
    if (std::find(reported.begin(), reported.end(), seen) != reported.end()) {
      continue;
    }
    reported.push_back(seen);
    Log.warn("{}: {} uploading {}x{} ({} levels, pixels={})", label, gl_error_name(e), w, h, levels,
             hasPixels ? "yes" : "no");
  }
  if (!any) {
    return;
  }
  ++failures;
  if ((failures % 512) == 0) {
    Log.warn("{}: {} upload failures so far ({} distinct)", label, failures, reported.size());
  }
}

GLenum gl_wrap(GXTexWrapMode mode) noexcept {
  switch (mode) {
  case GX_REPEAT:
    return GL_REPEAT;
  case GX_MIRROR:
    return GL_MIRRORED_REPEAT;
  case GX_CLAMP:
  default:
    return GL_CLAMP_TO_EDGE;
  }
}

GLenum gl_min_filter(GXTexFilter filter, bool mips) noexcept {
  switch (filter) {
  case GX_NEAR:
    return GL_NEAREST;
  case GX_NEAR_MIP_NEAR:
    return mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
  case GX_LIN_MIP_NEAR:
    return mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
  case GX_NEAR_MIP_LIN:
    return mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
  case GX_LIN_MIP_LIN:
    return mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
  case GX_LINEAR:
  default:
    return GL_LINEAR;
  }
}

GLenum gl_mag_filter(GXTexFilter filter) noexcept {
  // GX only has near/linear magnification; mip filter names are treated by their base filter.
  switch (filter) {
  case GX_NEAR:
  case GX_NEAR_MIP_NEAR:
  case GX_NEAR_MIP_LIN:
    return GL_NEAREST;
  default:
    return GL_LINEAR;
  }
}

void set_wrap_filter(TextureRef& ref, GLenum wrapS, GLenum wrapT, GLenum minFilter, GLenum magFilter) noexcept {
  if (ref.glWrapS != wrapS) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapS));
    ref.glWrapS = static_cast<uint16_t>(wrapS);
  }
  if (ref.glWrapT != wrapT) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapT));
    ref.glWrapT = static_cast<uint16_t>(wrapT);
  }
  if (ref.glMinFilter != minFilter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(minFilter));
    ref.glMinFilter = static_cast<uint16_t>(minFilter);
  }
  if (ref.glMagFilter != magFilter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(magFilter));
    ref.glMagFilter = static_cast<uint16_t>(magFilter);
  }
}

// Creates a texture with `levels` levels of w x h RGBA8 (pixels == nullptr leaves the contents
// undefined, for render targets and dynamic textures). Leaves the new texture bound.
TextureHandle create_texture(uint32_t w, uint32_t h, uint32_t levels, const uint32_t* pixels, u32 gxFormat,
                             bool renderTarget, const char* label) noexcept {
  if (w == 0 || h == 0 || w > MaxTextureDim || h > MaxTextureDim || levels == 0) {
    Log.warn("{}: unsupported texture size {}x{} ({} levels)", label, w, h, levels);
    return {};
  }
  clear_gl_errors();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    Log.warn("{}: glGenTextures failed", label);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  set_unpack_alignment(4);

  auto ref = std::make_shared<TextureRef>();
  ref->glTexture = tex; // from here on the destructor queues the deletion on any failure
  ref->size = {w, h, 1};
  ref->mipCount = levels;
  ref->gxFormat = gxFormat;
  ref->isRenderTarget = renderTarget;
  sLiveBytes.fetch_add(chain_bytes(w, h, levels), std::memory_order_relaxed);

  // Defaults make the texture complete whatever mip chain it ends up with; the replay changes them
  // per draw with apply_sampler.
  set_wrap_filter(*ref, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_LINEAR, GL_LINEAR);

  size_t offset = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t lw = std::max(w >> level, 1u);
    const uint32_t lh = std::max(h >> level, 1u);
    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA, static_cast<GLsizei>(lw),
                 static_cast<GLsizei>(lh), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels != nullptr ? pixels + offset : nullptr);
    offset += static_cast<size_t>(lw) * lh;
  }
  report_gl_errors(label, w, h, levels, pixels != nullptr);
  return ref;
}

// Number of levels to keep for a texture of this shape: mip chains only on power-of-two textures
// (vitaGL's NPOT mip chains are broken), completed down to 1x1 so mip filters are usable.
uint32_t levels_to_decode(uint32_t w, uint32_t h, uint32_t mips) noexcept {
  if (mips <= 1 || !is_pow2(w) || !is_pow2(h)) {
    return 1;
  }
  return std::min(mips, full_mip_count(w, h));
}
uint32_t levels_to_keep(uint32_t w, uint32_t h, uint32_t decoded) noexcept {
  return decoded > 1 ? full_mip_count(w, h) : 1;
}

GLuint compile_shader(GLenum type, const char* source) noexcept {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Log.error("palette conversion shader failed to compile: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

constexpr const char PaletteVertexShader[] = R"(
attribute vec2 aPos;
varying vec2 vUV;
void main() {
  gl_Position = vec4(aPos, 0.0, 1.0);
  vUV = aPos * 0.5 + 0.5;
}
)";
// dst(x,y) = tlut[round(src(x,y).r * scale)]; indices past the TLUT read as transparent black.
constexpr const char PaletteFragmentShader[] = R"(
precision highp float;
varying vec2 vUV;
uniform sampler2D uSrc;
uniform sampler2D uTlut;
uniform vec2 uParams; // x = index scale (255 or 15), y = TLUT entries
void main() {
  float idx = floor(texture2D(uSrc, vUV).r * uParams.x + 0.5);
  if (idx >= uParams.y) {
    gl_FragColor = vec4(0.0);
  } else {
    gl_FragColor = texture2D(uTlut, vec2((idx + 0.5) / uParams.y, 0.5));
  }
}
)";

bool ensure_palette_program() noexcept {
  if (sPalette.program != 0) {
    return true;
  }
  if (sPalette.failed) {
    return false;
  }
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, PaletteVertexShader);
  const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, PaletteFragmentShader);
  if (vs == 0 || fs == 0) {
    sPalette.failed = true;
    return false;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "aPos");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    Log.error("palette conversion program failed to link");
    glDeleteProgram(program);
    sPalette.failed = true;
    return false;
  }
  sPalette.program = program;
  sPalette.uSrc = glGetUniformLocation(program, "uSrc");
  sPalette.uTlut = glGetUniformLocation(program, "uTlut");
  sPalette.uParams = glGetUniformLocation(program, "uParams");
  glGenFramebuffers(1, &sPalette.fbo);
  return true;
}

void run_palette_pass(TextureRef& dst, TextureRef& src, TextureRef& tlut, bool fourBit,
                      uint32_t restoreFramebuffer) noexcept {
  glBindFramebuffer(GL_FRAMEBUFFER, sPalette.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst.glTexture, 0);
  glViewport(0, 0, static_cast<GLsizei>(dst.size.width), static_cast<GLsizei>(dst.size.height));
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);

  glUseProgram(sPalette.program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src.glTexture);
  set_wrap_filter(src, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tlut.glTexture);
  set_wrap_filter(tlut, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST);
  glUniform1i(sPalette.uSrc, 0);
  glUniform1i(sPalette.uTlut, 1);
  glUniform2f(sPalette.uParams, fourBit ? 15.f : 255.f, static_cast<float>(tlut.size.width));

  static const GLfloat kTriangle[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kTriangle);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(0);

  glDepthMask(GL_TRUE);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, restoreFramebuffer);
}

} // namespace

// ---- TextureRef --------------------------------------------------------------------------------

TextureRef::~TextureRef() {
  if (glTexture == 0 && glFbo == 0) {
    return;
  }
  if (glTexture != 0) {
    sLiveBytes.fetch_sub(chain_bytes(size.width, size.height, mipCount), std::memory_order_relaxed);
  }
  std::lock_guard lock{delete_mutex()};
  pending_deletes().push_back({glTexture, glFbo});
}

void flush_texture_deletions() noexcept {
  std::vector<PendingDelete> pending;
  {
    std::lock_guard lock{delete_mutex()};
    pending.swap(pending_deletes());
  }
  if (pending.empty()) {
    return;
  }
  std::vector<GLuint> textures;
  textures.reserve(pending.size());
  for (const auto& p : pending) {
    if (p.fbo != 0) {
      glDeleteFramebuffers(1, &p.fbo);
    }
    if (p.texture != 0) {
      textures.push_back(p.texture);
    }
  }
  if (!textures.empty()) {
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
  }
}

void shutdown_textures() noexcept {
  sEmptyTexture.reset();
  if (sPalette.program != 0) {
    glDeleteProgram(sPalette.program);
  }
  if (sPalette.fbo != 0) {
    glDeleteFramebuffers(1, &sPalette.fbo);
  }
  sPalette = {};
  std::vector<uint32_t>().swap(sScratch);
  flush_texture_deletions();
}

uint64_t texture_gpu_bytes(const TextureRef& ref) noexcept {
  return ref.glTexture != 0 ? chain_bytes(ref.size.width, ref.size.height, ref.mipCount) : 0;
}
uint64_t live_texture_bytes() noexcept { return sLiveBytes.load(std::memory_order_relaxed); }

// ---- sampler -----------------------------------------------------------------------------------

void apply_sampler(TextureRef& ref, const SamplerDesc& sampler) noexcept {
  const bool mips = ref.mipCount > 1;
  set_wrap_filter(ref, gl_wrap(sampler.wrapS), gl_wrap(sampler.wrapT), gl_min_filter(sampler.minFilter, mips),
                  gl_mag_filter(sampler.magFilter));
}

// ---- creation ----------------------------------------------------------------------------------

TextureHandle new_texture_rgba8(uint32_t width, uint32_t height, uint32_t levels, const uint32_t* pixels, u32 gxFormat,
                                const char* label) noexcept {
  if (pixels == nullptr || width == 0 || height == 0) {
    return {};
  }
  const uint32_t have = levels_to_decode(width, height, levels);
  const uint32_t keep = levels_to_keep(width, height, have);
  if (keep == have) {
    return create_texture(width, height, keep, pixels, gxFormat, false, label);
  }
  // Complete the chain in a scratch copy (only the levels actually present are read from `pixels`).
  sScratch.assign(pixels, pixels + rgba8_mip_texels(width, height, have));
  extend_mip_chain(sScratch, width, height, have, keep);
  auto handle = create_texture(width, height, keep, sScratch.data(), gxFormat, false, label);
  trim_scratch();
  return handle;
}

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {
  if (tlut) {
    if (height != 1) {
      Log.warn("{}: expected TLUT height 1, got {}", label, height);
      return {};
    }
    GXTlutFmt format = GX_TL_IA8;
    switch (gxFormat) {
    case GX_TF_RGB565:
      format = GX_TL_RGB565;
      break;
    case GX_TF_RGB5A3:
      format = GX_TL_RGB5A3;
      break;
    case GX_TF_IA8:
      break;
    default:
      Log.warn("{}: unsupported TLUT texture format {}", label, gxFormat);
      return {};
    }
    if (!decode_tlut(format, width, data.data(), data.size(), sScratch)) {
      Log.warn("{}: TLUT with {} entries has too little data ({} bytes)", label, width, data.size());
      return {};
    }
    auto handle = create_texture(width, 1, 1, sScratch.data(), gxFormat, false, label);
    trim_scratch();
    return handle;
  }

  if (gxFormat == InvalidTextureFormat || !is_supported_texture_format(gxFormat) || gxFormat == GX_TF_C4 ||
      gxFormat == GX_TF_C8 || gxFormat == GX_TF_C14X2) {
    Log.warn("{}: unsupported texture format {:#x}", label, gxFormat);
    return {};
  }
  const uint32_t decodeLevels = levels_to_decode(width, height, mips);
  bool arbitraryMips = false;
  if (!decode_texture(gxFormat, width, height, decodeLevels, data.data(), data.size(), sScratch,
                      DetectArbitraryMips ? &arbitraryMips : nullptr)) {
    Log.warn("{}: failed to decode {}x{} format {:#x} texture from {} bytes", label, width, height, gxFormat,
             data.size());
    return {};
  }
  const uint32_t keep = levels_to_keep(width, height, decodeLevels);
  extend_mip_chain(sScratch, width, height, decodeLevels, keep);
  auto handle = create_texture(width, height, keep, sScratch.data(), gxFormat, false, label);
  trim_scratch();
  if (handle) {
    handle->hasArbitraryMips = arbitraryMips;
  }
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  const uint32_t levels = levels_to_keep(width, height, levels_to_decode(width, height, mips));
  return create_texture(width, height, levels, nullptr, gxFormat, false, label);
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  if (ref.glTexture == 0) {
    return;
  }
  const u32 format = ref.gxFormat == InvalidTextureFormat ? static_cast<u32>(GX_TF_RGBA8_PC) : ref.gxFormat;
  // The source may carry fewer levels than the GL texture has (the rest are regenerated).
  uint32_t have = ref.mipCount;
  while (have > 1 && gx_texture_source_size(format, ref.size.width, ref.size.height, have) > data.size()) {
    --have;
  }
  if (!decode_texture(format, ref.size.width, ref.size.height, have, data.data(), data.size(), sScratch, nullptr)) {
    Log.warn("write_texture: failed to decode {}x{} format {:#x} texture from {} bytes", ref.size.width,
             ref.size.height, format, data.size());
    return;
  }
  extend_mip_chain(sScratch, ref.size.width, ref.size.height, have, ref.mipCount);
  glBindTexture(GL_TEXTURE_2D, ref.glTexture);
  set_unpack_alignment(4);
  size_t offset = 0;
  for (uint32_t level = 0; level < ref.mipCount; ++level) {
    const uint32_t lw = std::max(ref.size.width >> level, 1u);
    const uint32_t lh = std::max(ref.size.height >> level, 1u);
    glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(lw),
                    static_cast<GLsizei>(lh), GL_RGBA, GL_UNSIGNED_BYTE, sScratch.data() + offset);
    offset += static_cast<size_t>(lw) * lh;
  }
  ++ref.contentSerial;
  trim_scratch();
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  return create_texture(width, height, 1, nullptr, gxFormat, true, label);
}

// The replay converts to the requested copy format while resolving, so a "conversion" texture is an
// ordinary RGBA8 render target here (see gfx/tex_copy_conv.hpp).
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  return create_texture(width, height, 1, nullptr, gxFormat, true, label);
}

TextureHandle new_palette_derived_texture(TextureHandle src, TextureHandle tlut, bool fourBit,
                                          const char* label) noexcept {
  if (!src || !tlut) {
    return {};
  }
  auto handle = create_texture(src->size.width, src->size.height, 1, nullptr, GX_TF_RGBA8, true, label);
  if (!handle) {
    return {};
  }
  auto derived = std::make_shared<PaletteDerivation>();
  derived->src = std::move(src);
  derived->tlut = std::move(tlut);
  derived->fourBit = fourBit;
  handle->derived = std::move(derived);
  return handle;
}

void update_palette_derivation(TextureRef& dst, TextureHandle tlut) noexcept {
  if (!dst.derived || dst.derived->tlut == tlut) {
    return;
  }
  dst.derived->tlut = std::move(tlut);
  dst.derived->converted = false;
}

void mark_texture_written(TextureRef& ref) noexcept { ++ref.contentSerial; }

bool prepare_texture_for_sampling(TextureRef& ref, uint32_t restoreFramebuffer) noexcept {
  if (!ref.derived) {
    return false;
  }
  PaletteDerivation& d = *ref.derived;
  if (!d.src || !d.tlut || d.src->glTexture == 0 || d.tlut->glTexture == 0 || ref.glTexture == 0) {
    return false;
  }
  if (d.converted && d.convertedSerial == d.src->contentSerial) {
    return false;
  }
  if (!ensure_palette_program()) {
    return false;
  }
  run_palette_pass(ref, *d.src, *d.tlut, d.fourBit, restoreFramebuffer);
  d.converted = true;
  d.convertedSerial = d.src->contentSerial;
  ++ref.contentSerial;
  return true;
}

TextureHandle empty_texture() noexcept {
  if (!sEmptyTexture) {
    static const uint32_t kPixel = 0;
    sEmptyTexture = create_texture(1, 1, 1, &kPixel, GX_TF_RGBA8, false, "Empty texture");
  }
  return sEmptyTexture;
}

} // namespace aurora::gfx
