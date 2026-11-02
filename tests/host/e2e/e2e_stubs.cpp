// Host stand-ins for the few engine services the GX frontend and the GL replay call that are not part of the
// pipeline under test: logging, the global config, the VI framebuffer size and the window.
#include <internal.hpp>
#include <logging.hpp>
#include <window.hpp>
#include <dolphin/vi/vi_internal.hpp>

#include <cstdio>
#include <cstdlib>

namespace aurora {
AuroraConfig g_config{};

// AuroraLogLevel: 0 debug .. 3 error, 4 fatal. Warnings and worse always print; TPV_E2E_VERBOSE=1 prints everything.
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int) noexcept {
  static const bool verbose = std::getenv("TPV_E2E_VERBOSE") != nullptr;
  if (verbose || static_cast<int>(level) >= static_cast<int>(LOG_WARNING)) {
    std::fprintf(stderr, "[%d] %s: %s\n", static_cast<int>(level), module, message);
  }
}

namespace vi {
Vec2<uint32_t> configured_fb_size() noexcept { return {640, 480}; }
} // namespace vi

namespace window {
AuroraWindowSize get_window_size() {
  AuroraWindowSize size{};
  size.width = size.native_fb_width = size.fb_width = 640;
  size.height = size.native_fb_height = size.fb_height = 480;
  size.scale = 1.f;
  return size;
}
void set_frame_buffer_aspect_fit(bool) {}
} // namespace window
} // namespace aurora
