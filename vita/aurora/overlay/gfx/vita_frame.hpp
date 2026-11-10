// Vita backend: per-frame recording consumed by the GL replay.
//
// The GX command processor appends to a Frame (raw big-endian vertex bytes, u16 indices, packed
// uniform blobs, snapshotted vertex arrays, and an ordered op list). At the end of the frame the
// op list is handed to vita::replay(), which owns every GL call.
#pragma once

#include "recording.hpp"
#include "../gx/pipeline.hpp"

#include <vector>

namespace aurora::gfx::vita {

struct Op {
  enum class Kind : uint8_t { Draw, Viewport, Scissor, Resolve, BeginOffscreen, EndOffscreen };
  Kind kind = Kind::Draw;
  uint32_t index = 0; // Draw: Frame::draws, Resolve: Frame::resolves
  Viewport viewport{};
  ClipRect scissor{};
  uint32_t width = 0;  // BeginOffscreen
  uint32_t height = 0;
};

struct ResolveOp {
  TextureHandle texture;
  ClipRect rect;
  bool clearColor;
  bool clearAlpha;
  bool clearDepth;
  Vec4<float> clearColorValue;
  float clearDepthValue;
  GXTexFmt resolveFormat;
  GXPixelFmt sourceFormat;
};

struct Frame {
  std::vector<uint8_t> verts;    // raw GX vertex data, big-endian, as the game wrote it
  std::vector<uint8_t> indices;  // u16 triangle indices relative to the draw's first vertex
  std::vector<uint8_t> uniforms; // gx::fill_uniform blobs (vec4 multiples)
  std::vector<uint8_t> storage;  // snapshots of GXSetArray vertex arrays
  std::vector<gx::DrawData> draws;
  std::vector<ResolveOp> resolves;
  std::vector<TextureBindSet> bindGroups;
  std::vector<Op> ops;
  uint64_t renderTargetKey = 0; // changes with begin/end offscreen
  // EFB clear at the start of the frame (what upstream's first render pass loads with).
  Vec4<float> clearColor{0.f, 0.f, 0.f, 0.f};
  float clearDepth = 1.f;
};

Frame& frame();
// Start recording a frame: captures the EFB clear values and the initial viewport/scissor.
void begin_frame();
// Looks up what a draw's PipelineRef / BindGroupRef stand for. Pipeline configs outlive frames.
const gx::PipelineConfig* find_pipeline(PipelineRef ref);
const TextureBindSet* find_bind_group(BindGroupRef ref);

// ---- implemented by the GL renderer (vita_gl_replay.cpp) -------------------------------
// Size of the main render target as the GAME defines it: the VI render mode (608x448 for Twilight
// Princess). Viewports and scissors the game sets are in these units.
Vec2<uint32_t> main_target_size() noexcept;
// The EFB actually allocated, in pixels: main_target_size() reduced by the render scale. This is what
// the GL replay binds, and what gfx::get_render_target_size() reports for the main target.
Vec2<uint32_t> efb_target_size() noexcept;
// Execute a recorded frame into the EFB. Called on the thread that owns the GL context.
void replay(Frame& frame);
// Blit the EFB to the default framebuffer, aspect-fit and letterboxed. The caller swaps buffers.
void present();
// Size of the default framebuffer (960x544 on Vita) that present() targets.
void set_present_size(uint32_t width, uint32_t height) noexcept;
// Directory where the source of a shader that fails to compile is saved (at most 8 pairs of files), so that a
// first run on a Vita leaves the exact text the translator refused. Empty or null = off (the default).
void set_shader_dump_dir(const char* dir) noexcept;
// Releases every GL object owned by the replay.
void shutdown_gl();
// Reads the EFB back as RGBA8, top row first (tests / screenshots). Returns false if no EFB exists.
bool read_efb_rgba(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height);
} // namespace aurora::gfx::vita
