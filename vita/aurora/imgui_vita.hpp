#pragma once

#include <imgui.h>

#include <cstdint>

namespace aurora::vita::imgui {
// Creates the ImGui context (no SDL / renderer backend; see imgui_vita.cpp).
void create_context(const char* iniPath);
// Starts an ImGui frame (builds/uploads the font atlas when the fonts changed).
void new_frame(uint32_t width, uint32_t height);
// Ends the frame and draws it into the default framebuffer (after the EFB has been presented).
void render_and_draw(uint32_t width, uint32_t height);
ImTextureID add_texture(uint32_t width, uint32_t height, const void* rgba8);
void shutdown();
} // namespace aurora::vita::imgui
