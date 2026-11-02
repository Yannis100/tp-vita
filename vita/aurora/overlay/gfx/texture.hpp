// Vita overlay of aurora/lib/gfx/texture.hpp: GX texture/TLUT object wrappers are unchanged;
// the texture resource is a vitaGL texture instead of a wgpu::Texture.
#pragma once
#include <dolphin/gx.h>

#include <utility>

#include "types.hpp"

namespace aurora::gfx {
constexpr u32 InvalidTextureFormat = -1;

// Sampler state of one GX texture binding, already translated to what the GL backend needs.
struct SamplerDesc {
  GXTexWrapMode wrapS = GX_CLAMP;
  GXTexWrapMode wrapT = GX_CLAMP;
  GXTexFilter minFilter = GX_LINEAR;
  GXTexFilter magFilter = GX_LINEAR;
  float minLod = 0.f;
  float maxLod = 0.f;
  float lodBias = 0.f;
  bool operator==(const SamplerDesc&) const = default;
};

struct PaletteDerivation; // gfx/vita_texture.hpp

// A GL texture owned by the Vita backend. glTexture is a GLuint (0 = none); it is deleted when
// the last handle goes away (on the GL thread).
struct TextureRef {
  uint32_t glTexture = 0;
  Extent3D size;
  uint32_t mipCount = 1;
  u32 gxFormat = InvalidTextureFormat;
  bool hasArbitraryMips = false;
  bool isReplacement = false;
  // True for EFB copy destinations: the texture is rendered to, not uploaded from CPU data.
  bool isRenderTarget = false;
  // Framebuffer object the replay attaches this texture to when an EFB copy renders into it
  // (created lazily by the replay, deleted together with glTexture; 0 = none).
  uint32_t glFbo = 0;
  // GL sampler parameters last applied to this texture by apply_sampler() (GLenum values; 0 = never
  // applied). Owned by vita_texture.cpp; anything else that changes them must update these.
  uint16_t glWrapS = 0;
  uint16_t glWrapT = 0;
  uint16_t glMinFilter = 0;
  uint16_t glMagFilter = 0;
  // Bumped whenever the contents are rendered to (vita::mark_texture_written); used to refresh
  // derived textures lazily.
  uint32_t contentSerial = 0;
  // Set for textures whose contents are computed on the GPU from another texture (an EFB copy sampled
  // through a TLUT); see gfx/vita_texture.hpp.
  std::shared_ptr<PaletteDerivation> derived;

  TextureRef() = default;
  TextureRef(const TextureRef&) = delete;
  TextureRef& operator=(const TextureRef&) = delete;
  ~TextureRef();
};

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept;
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept;
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept;
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept;
void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept;
} // namespace aurora::gfx

struct GXTexObj_ {
  u32 mode0 = 0;
  u32 mode1 = 0;
  u32 image0 = UINT32_MAX;
  u32 image3 = 0;
  const void* userData = nullptr;
  const void* data = nullptr;
  u32 mWidth = 0;
  u32 mHeight = 0;
  u32 mFormat = aurora::gfx::InvalidTextureFormat;
  GXTlut tlut = GX_TLUT0;
  u32 texObjId = 0;
  u32 texDataVersion = 0;
  u8 flags = 0;

  static constexpr u32 get_bits(u32 reg, u32 size, u32 shift) noexcept { return (reg >> shift) & ((1u << size) - 1); }

  u32 width() const noexcept { return mWidth != 0 ? mWidth : image0 == UINT32_MAX ? 0 : get_bits(image0, 10, 0) + 1; }
  u32 height() const noexcept {
    return mHeight != 0 ? mHeight : image0 == UINT32_MAX ? 0 : get_bits(image0, 10, 10) + 1;
  }
  u32 raw_format() const noexcept { return get_bits(image0, 4, 20); }
  u32 format() const noexcept { return mFormat != aurora::gfx::InvalidTextureFormat ? mFormat : raw_format(); }
  GXTexWrapMode wrap_s() const noexcept { return static_cast<GXTexWrapMode>(get_bits(mode0, 2, 0)); }
  GXTexWrapMode wrap_t() const noexcept { return static_cast<GXTexWrapMode>(get_bits(mode0, 2, 2)); }
  GXTexFilter min_filter() const noexcept {
    constexpr GXTexFilter kHwToGxFilter[8] = {
        GX_NEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR, GX_LINEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN, GX_NEAR,
    };
    return kHwToGxFilter[get_bits(mode0, 3, 5)];
  }
  GXTexFilter mag_filter() const noexcept { return get_bits(mode0, 1, 4) != 0 ? GX_LINEAR : GX_NEAR; }
  GXBool has_mips() const noexcept { return (flags & 1u) != 0 ? GX_TRUE : GX_FALSE; }
  u32 mip_count() const noexcept { return has_mips() ? std::max<u32>(static_cast<u32>(max_lod()) + 1, 1u) : 1; }
  GXBool do_edge_lod() const noexcept { return get_bits(mode0, 1, 8) == 0 ? GX_TRUE : GX_FALSE; }
  float lod_bias() const noexcept { return static_cast<float>(static_cast<int8_t>(get_bits(mode0, 8, 9))) / 32.0f; }
  GXAnisotropy max_aniso() const noexcept { return static_cast<GXAnisotropy>(get_bits(mode0, 2, 19)); }
  GXBool bias_clamp() const noexcept { return get_bits(mode0, 1, 21) != 0 ? GX_TRUE : GX_FALSE; }
  float min_lod() const noexcept { return static_cast<float>(get_bits(mode1, 8, 0)) / 16.0f; }
  float max_lod() const noexcept { return static_cast<float>(get_bits(mode1, 8, 8)) / 16.0f; }

  // Custom flag for texture caching
  bool no_cache() const noexcept { return (flags & 0x80) != 0; }
  void set_no_cache(bool value) noexcept { flags = value ? flags | 0x80 : flags & ~0x80; }

  // Hacky workaround for an instances where incremental IDs are used for GXCopyTex, but the copy tex was invalidated
  // and the texture reference is still present.
  bool has_data() const noexcept { return reinterpret_cast<uintptr_t>(data) >= 0x10000; }
};
static_assert(sizeof(GXTexObj_) <= sizeof(GXTexObj), "GXTexObj too small!");
struct GXTlutObj_ {
  u32 tlut = 0;
  u32 loadTlut0 = 0;
  u16 numEntries = 0;
  const void* data = nullptr;
  GXTlutFmt format = GX_TL_IA8;
  u32 tlutObjId = 0;
  u32 tlutDataVersion = 0;
  u8 flags = 0;

  // Custom flag for texture caching
  bool no_cache() const noexcept { return (flags & 0x80) != 0; }
  void set_no_cache(bool value) noexcept { flags = value ? flags | 0x80 : flags & ~0x80; }
};
static_assert(sizeof(GXTlutObj_) <= sizeof(GXTlutObj), "GXTlutObj too small!");

namespace aurora::gfx {
struct TextureBind {
  TextureHandle ref;
  GXTexObj_ texObj;
  uint64_t generation = 0;

  TextureBind() noexcept = default;
  TextureBind(const GXTexObj_& obj, TextureHandle handle, uint64_t bindGeneration = 0) noexcept
  : ref(std::move(handle)), texObj(obj), generation(bindGeneration) {}
  void reset() noexcept { ref.reset(); }
  [[nodiscard]] SamplerDesc get_descriptor() const noexcept {
    return {
        .wrapS = texObj.wrap_s(),
        .wrapT = texObj.wrap_t(),
        .minFilter = texObj.min_filter(),
        .magFilter = texObj.mag_filter(),
        .minLod = texObj.min_lod(),
        .maxLod = texObj.max_lod(),
        .lodBias = texObj.lod_bias(),
    };
  }
  operator bool() const noexcept { return ref.operator bool(); }
};
} // namespace aurora::gfx
