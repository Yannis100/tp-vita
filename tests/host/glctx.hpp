// Host-side OpenGL ES 2 context for testing the Vita renderer pieces without a Vita.
//
// Uses Mesa's software rasterizer through EGL (surfaceless platform + pbuffer). GL headers come
// from the VitaSDK (GLES2/KHR); EGL is declared by hand because the SDK's eglext.h is Vita-specific.
// Build the tests with:  tests/host/build.sh <out> <sources...>
#pragma once

#include <GLES2/gl2.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hostgl {

// Creates a GLES2 context with a w x h pbuffer. Returns false (and prints why) on failure.
bool init(int width = 640, int height = 480);
void shutdown();

// Compiles a GLSL ES shader; returns true on success, otherwise fills `log`.
bool compile(GLenum type, const std::string& source, std::string& log);
// Compiles and links a vertex+fragment pair (after binding attribute names to locations 0..n-1
// in the order given); returns the program or 0 and fills `log`.
GLuint link(const std::string& vs, const std::string& fs, const std::vector<std::string>& attribNames,
            std::string& log);

// Reads back the current framebuffer as RGBA8 (top row first).
std::vector<uint8_t> read_rgba(int width, int height);
// Writes RGBA8 pixels (top row first) as a PNG.
bool write_png(const char* path, const uint8_t* rgba, int width, int height);

} // namespace hostgl
