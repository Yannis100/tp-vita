// Vita overlay of aurora/lib/gfx/recording.hpp.
//
// Upstream records GX draws into a frame packet on the FIFO thread and replays them into WebGPU
// render passes on a render worker. The Vita backend keeps the recording half (CPU bump buffers +
// an ordered op list, so the command processor's draw merging still works) and replays the list
// with vitaGL at the end of the frame, on the thread that owns the GL context.
#pragma once

#include "texture.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace aurora::gfx {
// Render target the recorded draws land in. `key` changes whenever the target changes shape.
struct RenderTargetLayout {
  uint64_t key = 0;
};

namespace detail {
void increment_merged_draw_count() noexcept;
} // namespace detail

// Textures sampled by one draw, snapshotted when the draw is recorded (the GL objects stay alive
// until the frame has been replayed).
struct BoundTexture {
  TextureHandle ref;
  SamplerDesc sampler;
};
struct TextureBindSet {
  std::array<BoundTexture, GX_MAX_TEXMAP> slots;
};
BindGroupRef bind_group_ref(const TextureBindSet& set);

void finish();
bool is_offscreen() noexcept;
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
bool has_normal_attachment() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}

// Only gx::DrawData / gx::PipelineConfig are supported; defined (and explicitly instantiated) in
// the Vita frame recorder.
template <typename DrawData>
void push_draw_command(DrawData data);
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       GXPixelFmt sourceFormat);
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);
} // namespace aurora::gfx
