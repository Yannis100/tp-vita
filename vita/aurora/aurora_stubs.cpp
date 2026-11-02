/**
 * Vita port: placeholder definitions for the parts of Aurora that sit on top of the WebGPU/SDL
 * backend (window management, RmlUi renderer glue, texture replacement, ImGui textures).
 * Each entry point keeps the upstream signature so the game links; they do nothing until the
 * Vita backend (M2) provides real implementations.
 */
#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <aurora/rmlui.hpp>
#include <dolphin/gx.h>  // GXTexObj/GXTlutObj, which texture.hpp expects its includer to provide
#include <aurora/texture.hpp>

#include "window.hpp"  // ${AURORA_DIR}/lib

namespace aurora {
AuroraConfig g_config{};
char g_gameName[4]{};
}  // namespace aurora

namespace aurora::window {
AuroraWindowSize get_window_size() {
    AuroraWindowSize size{};
    size.width = 960;
    size.height = 544;
    size.fb_width = 960;
    size.fb_height = 544;
    size.native_fb_width = 960;
    size.native_fb_height = 544;
    size.scale = 1.0f;
    return size;
}
SDL_Window* get_sdl_window() { return nullptr; }
void set_title(const char*) {}
void set_fullscreen(bool) {}
bool get_fullscreen() { return true; }
void set_window_size(uint32_t, uint32_t) {}
void set_window_position(uint32_t, uint32_t) {}
void center_window() {}
void request_frame_buffer_resize() {}
void set_frame_buffer_aspect_fit(bool) {}
void set_frame_buffer_scale(float) {}
}  // namespace aurora::window

namespace aurora::rmlui {
Rml::Context* get_context() noexcept { return nullptr; }
bool is_initialized() noexcept { return false; }
void set_input_type(InputType) noexcept {}
void set_ui_scale(float) noexcept {}
float get_ui_scale() noexcept { return 1.0f; }
void set_glass_light_dir(float, float) noexcept {}
void register_texture_provider(std::string, TextureProvider) {}
void unregister_texture_provider(std::string_view) noexcept {}
}  // namespace aurora::rmlui

namespace aurora::texture {
std::optional<TextureSourceKey> parse_replacement_filename(std::string_view) noexcept {
    return std::nullopt;
}
ReplacementRegistration register_virtual_replacement(std::string_view, VirtualFileSource,
                                                     ReplacementOptions) {
    return {};
}
ReplacementRegistration register_replacement(ReplacementKey, RawTextureReplacement,
                                             ReplacementOptions) {
    return {};
}
void unregister_replacement(const ReplacementRegistration&) {}
void unregister_replacements(std::span<const ReplacementRegistration>) {}
void unregister_replacements(const ReplacementGroup&) {}
void unregister_replacements(const ReplacementKey&) {}
void clear_replacements() {}
ReplacementGroup load_replacement_directory(const std::filesystem::path&, ReplacementOptions) {
    return {};
}
void reload_replacement_directory(const std::filesystem::path&, ReplacementGroup&,
                                  ReplacementOptions) {}
bool has_replacement(const GXTexObj*, const GXTlutObj*) { return false; }
}  // namespace aurora::texture
