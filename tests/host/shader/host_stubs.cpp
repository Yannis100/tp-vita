// Link stubs so shader_info.cpp (build_shader_info) can be used on the host without the GX frontend.
// Only symbols that build_shader_info's translation unit references through fill_uniform/logging.
#include <gfx/recording.hpp>
#include <gfx/texture.hpp>
#include <gx/gx.hpp>
#include <logging.hpp>

#include <cstdio>

namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int) noexcept {
  std::fprintf(stderr, "[%d] %s: %s\n", static_cast<int>(level), module, message);
}
namespace gfx {
Range push_uniform(const uint8_t*, size_t) { return {}; }
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
} // namespace gfx
namespace gx {
const gfx::TextureBind& get_texture(GXTexMapID) noexcept {
  static const gfx::TextureBind bind{};
  return bind;
}
} // namespace gx
} // namespace aurora

// g_gxState is only referenced by fill_uniform (never called here). Defining a real GXState would drag in
// abseil's hash-map runtime, so alias the mangled symbol to raw storage instead.
alignas(64) unsigned char g_gxStateStorage[sizeof(aurora::gx::GXState)] __asm__("_ZN6aurora2gx9g_gxStateE");
