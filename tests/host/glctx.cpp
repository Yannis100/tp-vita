#include "glctx.hpp"

#include <EGL/egl.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

namespace hostgl {
namespace {
EGLDisplay sDpy = EGL_NO_DISPLAY;
EGLContext sCtx = EGL_NO_CONTEXT;
EGLSurface sSurf = EGL_NO_SURFACE;
} // namespace

bool init(int width, int height) {
  using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLint*);
  auto getPlatformDisplay = reinterpret_cast<GetPlatformDisplay>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  sDpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (sDpy == EGL_NO_DISPLAY || !eglInitialize(sDpy, &major, &minor)) {
    std::fprintf(stderr, "hostgl: eglInitialize failed (0x%x)\n", eglGetError());
    return false;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE,     8,                EGL_GREEN_SIZE,      8,
                            EGL_BLUE_SIZE,    8,                EGL_ALPHA_SIZE,      8,
                            EGL_DEPTH_SIZE,   24,               EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(sDpy, cfgAttr, &cfg, 1, &n) || n < 1) {
    std::fprintf(stderr, "hostgl: no EGL config (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  sCtx = eglCreateContext(sDpy, cfg, EGL_NO_CONTEXT, ctxAttr);
  const EGLint pbAttr[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
  sSurf = eglCreatePbufferSurface(sDpy, cfg, pbAttr);
  if (sCtx == EGL_NO_CONTEXT || sSurf == EGL_NO_SURFACE || !eglMakeCurrent(sDpy, sSurf, sSurf, sCtx)) {
    std::fprintf(stderr, "hostgl: context creation failed (0x%x)\n", eglGetError());
    return false;
  }
  return true;
}

void shutdown() {
  if (sDpy == EGL_NO_DISPLAY) {
    return;
  }
  eglMakeCurrent(sDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (sSurf != EGL_NO_SURFACE) eglDestroySurface(sDpy, sSurf);
  if (sCtx != EGL_NO_CONTEXT) eglDestroyContext(sDpy, sCtx);
  eglTerminate(sDpy);
  sDpy = EGL_NO_DISPLAY;
}

bool compile(GLenum type, const std::string& source, std::string& log) {
  const GLuint shader = glCreateShader(type);
  const char* src = source.c_str();
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    log.assign(static_cast<size_t>(len > 0 ? len : 1), '\0');
    glGetShaderInfoLog(shader, len, nullptr, log.data());
  }
  glDeleteShader(shader);
  return ok != 0;
}

GLuint link(const std::string& vs, const std::string& fs, const std::vector<std::string>& attribNames,
            std::string& log) {
  const GLuint v = glCreateShader(GL_VERTEX_SHADER);
  const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  const GLuint prog = glCreateProgram();
  auto build = [&](GLuint sh, const std::string& s, const char* what) {
    const char* src = s.c_str();
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char buf[4096];
      glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
      log += std::string(what) + ": " + buf;
    }
    return ok != 0;
  };
  bool ok = build(v, vs, "vertex");
  ok = build(f, fs, "fragment") && ok;
  if (ok) {
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    for (size_t i = 0; i < attribNames.size(); ++i) {
      glBindAttribLocation(prog, static_cast<GLuint>(i), attribNames[i].c_str());
    }
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
      char buf[4096];
      glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
      log += std::string("link: ") + buf;
      ok = false;
    }
  }
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok) {
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

std::vector<uint8_t> read_rgba(int width, int height) {
  std::vector<uint8_t> bottomUp(static_cast<size_t>(width) * height * 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
  std::vector<uint8_t> out(bottomUp.size());
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.data() + static_cast<size_t>(y) * width * 4,
                bottomUp.data() + static_cast<size_t>(height - 1 - y) * width * 4, static_cast<size_t>(width) * 4);
  }
  return out;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put32(out, static_cast<uint32_t>(crc32(0, body.data(), static_cast<uInt>(body.size()))));
}

bool write_png(const char* path, const uint8_t* rgba, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (width * 4 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + static_cast<size_t>(y) * width * 4, rgba + static_cast<size_t>(y + 1) * width * 4);
  }
  uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> z(zlen);
  if (compress(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size())) != Z_OK) return false;
  z.resize(zlen);
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(width));
  put32(ihdr, static_cast<uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

} // namespace hostgl
