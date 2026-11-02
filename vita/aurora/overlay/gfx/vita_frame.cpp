// Vita backend: frame recorder (the half of upstream gfx/recording.cpp that does not touch WebGPU).
#include "vita_frame.hpp"

#include "../dolphin/vi/vi_internal.hpp"

#include <algorithm>
#include <unordered_map>

namespace aurora::gfx {
namespace {
using vita::Frame;
using vita::Op;

Frame sFrame;
std::unordered_map<PipelineRef, gx::PipelineConfig> sPipelines;
std::unordered_map<BindGroupRef, uint32_t> sBindGroupLookup; // hash -> index into Frame::bindGroups

struct Offscreen {
  bool active = false;
  uint32_t width = 0;
  uint32_t height = 0;
} sOffscreen;

size_t align_to(size_t value, size_t alignment) {
  return alignment <= 1 ? value : (value + alignment - 1) & ~(alignment - 1);
}

Range push_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t length, size_t alignment) {
  const size_t offset = align_to(buf.size(), alignment);
  buf.resize(offset + length);
  if (length != 0) {
    std::memcpy(buf.data() + offset, data, length);
  }
  return {static_cast<uint32_t>(offset), static_cast<uint32_t>(length)};
}

bool equal(const TextureBindSet& a, const TextureBindSet& b) {
  for (size_t i = 0; i < a.slots.size(); ++i) {
    if (a.slots[i].ref != b.slots[i].ref || !(a.slots[i].sampler == b.slots[i].sampler)) {
      return false;
    }
  }
  return true;
}
} // namespace

namespace vita {
Frame& frame() { return sFrame; }

void begin_frame() {
  // A vertex array is snapshotted into this frame's storage the first time a draw reads it, and the command processor keeps
  // that range until the array changes or GXInvalidateVtxCache runs. TP calls it once per frame (JFWDisplay::preGX), but the
  // range is an offset into storage that is rebuilt every frame: a cached one from the previous frame would point at bytes
  // that no longer exist and every indexed vertex would decode as zero. Drop them here instead of trusting the caller.
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  gx::g_gxState.dirty |= gx::DirtyImmediates;
  sFrame.clearColor = gx::g_gxState.clearColor;
  sFrame.clearDepth = gx::clear_depth_value();
  set_viewport(gx::map_logical_viewport(gx::g_gxState.logicalViewport));
  set_scissor(gx::map_logical_scissor(gx::g_gxState.logicalScissor));
}

const gx::PipelineConfig* find_pipeline(PipelineRef ref) {
  const auto it = sPipelines.find(ref);
  return it == sPipelines.end() ? nullptr : &it->second;
}

const TextureBindSet* find_bind_group(BindGroupRef ref) {
  // refs are index + 1 so that 0 stays "no textures"
  return ref == 0 || ref > sFrame.bindGroups.size() ? nullptr : &sFrame.bindGroups[ref - 1];
}
} // namespace vita

namespace detail {
void increment_merged_draw_count() noexcept {}
} // namespace detail

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  return push_bytes(sFrame.verts, data, length, alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  return push_bytes(sFrame.indices, data, length, alignment);
}
Range push_uniform(const uint8_t* data, size_t length) { return push_bytes(sFrame.uniforms, data, length, 16); }
Range push_storage(const uint8_t* data, size_t length) { return push_bytes(sFrame.storage, data, length, 16); }
uint32_t align_uniform(uint32_t value) { return static_cast<uint32_t>(align_to(value, 16)); }

template <>
void push_draw_command<gx::DrawData>(gx::DrawData data) {
  sFrame.draws.push_back(data);
  Op op;
  op.kind = Op::Kind::Draw;
  op.index = static_cast<uint32_t>(sFrame.draws.size() - 1);
  sFrame.ops.push_back(op);
}

template <>
gx::DrawData* get_last_draw_command<gx::DrawData>() {
  if (sFrame.ops.empty() || sFrame.ops.back().kind != Op::Kind::Draw) {
    return nullptr;
  }
  return &sFrame.draws[sFrame.ops.back().index];
}

template <>
PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
  HashType seed = 0;
  while (true) {
    const HashType hash = xxh3_hash(config, seed);
    const auto [it, inserted] = sPipelines.try_emplace(hash, config);
    if (inserted || memcmp(&it->second, &config, sizeof(config)) == 0) {
      return hash;
    }
    ++seed; // 32-bit hash collision between two different configs
  }
}

BindGroupRef bind_group_ref(const TextureBindSet& set) {
  Hasher hasher;
  for (const auto& slot : set.slots) {
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(slot.ref.get());
    hasher.update(&ptr, sizeof(ptr));
    hasher.update(&slot.sampler, sizeof(slot.sampler)); // floats: not hashable via update<T>
  }
  const auto key = static_cast<HashType>(hasher.digest());
  const auto it = sBindGroupLookup.find(key);
  if (it != sBindGroupLookup.end() && equal(sFrame.bindGroups[it->second], set)) {
    return it->second + 1;
  }
  sFrame.bindGroups.push_back(set);
  const auto index = static_cast<uint32_t>(sFrame.bindGroups.size() - 1);
  sBindGroupLookup[key] = index;
  return index + 1;
}

bool is_offscreen() noexcept { return sOffscreen.active; }

void begin_offscreen(uint32_t width, uint32_t height) {
  sOffscreen = {true, width, height};
  Op op;
  op.kind = Op::Kind::BeginOffscreen;
  op.width = width;
  op.height = height;
  sFrame.ops.push_back(op);
  sFrame.renderTargetKey = (uint64_t{width} << 32) | height | (1ull << 63);
}

void end_offscreen() {
  sOffscreen = {};
  Op op;
  op.kind = Op::Kind::EndOffscreen;
  sFrame.ops.push_back(op);
  sFrame.renderTargetKey = 0;
}

bool has_normal_attachment() noexcept { return false; }
RenderTargetLayout get_render_target_layout() noexcept { return {.key = sFrame.renderTargetKey}; }

Vec2<uint32_t> get_render_target_size() noexcept {
  return sOffscreen.active ? Vec2<uint32_t>{sOffscreen.width, sOffscreen.height} : vita::main_target_size();
}

void set_viewport(const Viewport& viewport) noexcept {
  Op op;
  op.kind = Op::Kind::Viewport;
  op.viewport = viewport;
  sFrame.ops.push_back(op);
}

void set_scissor(const ClipRect& scissor) noexcept {
  Op op;
  op.kind = Op::Kind::Scissor;
  op.scissor = scissor;
  sFrame.ops.push_back(op);
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       GXPixelFmt sourceFormat) {
  sFrame.resolves.push_back({std::move(texture), rect, clearColor, clearAlpha, clearDepth, clearColorValue,
                             clearDepthValue, resolveFormat, sourceFormat});
  Op op;
  op.kind = Op::Kind::Resolve;
  op.index = static_cast<uint32_t>(sFrame.resolves.size() - 1);
  sFrame.ops.push_back(op);
}

void push_debug_group(std::string) {}
void insert_debug_marker(std::string) {}
void clear_caches() noexcept {}

// End of recording: replay everything recorded since the last call, then reset the frame.
void finish() {
  vita::replay(sFrame);
  sFrame.verts.clear();
  sFrame.indices.clear();
  sFrame.uniforms.clear();
  sFrame.storage.clear();
  sFrame.draws.clear();
  sFrame.resolves.clear();
  sFrame.bindGroups.clear();
  sFrame.ops.clear();
  sBindGroupLookup.clear();
}
} // namespace aurora::gfx

// Public C debug-group API (aurora/gfx.h). Debug groups are not used by the GL replay.
void push_debug_group(const char*) {}
void pop_debug_group() {}
