// Vita overlay of aurora/lib/gx/texture.cpp.
//
// Same resolution logic as upstream (per-object caches keyed by texObjId + data version, a content cache
// keyed by a hash of the texture/TLUT bytes, TLUT and EFB-copy dependency tracking), with the WebGPU
// parts replaced:
//   - textures are decoded on the CPU to RGBA8 and uploaded to GL (gfx/vita_texture.cpp);
//   - palette textures backed by guest memory are expanded through the TLUT on the CPU;
//   - palette textures backed by an EFB copy become GPU-derived textures (gfx::new_palette_derived_texture),
//     refreshed lazily by the replay;
//   - texture replacements are not supported (the replacement API is stubbed out on the Vita).
// The content cache has a Vita-sized byte budget and idle object entries expire sooner under memory
// pressure.
#include "texture.hpp"

#include "shader_info.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/vita_texture.hpp"
#include "../gfx/vita_texture_convert.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <utility>
#include <vector>

namespace aurora::gx {
namespace {
constexpr Module Log{"aurora::gx::texture"};

// Decoded (RGBA8) bytes the content cache may retain. Textures a frame is still using stay alive through
// the recorded bind sets whether or not they are cached.
constexpr uint64_t VitaContentCacheBudgetBytes = 48ull * 1024ull * 1024ull;
// Frames an unused texture/TLUT object cache entry survives; shorter once the live GL textures exceed
// the soft limit. (Starting values; tune on a device.)
constexpr uint64_t VitaObjectIdleFrames = 300;
constexpr uint64_t VitaObjectIdleFramesUnderPressure = 30;
constexpr uint64_t VitaTextureSoftLimitBytes = 96ull * 1024ull * 1024ull;

struct DynamicPaletteKey {
  const void* sourceIdentity = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;

  bool operator==(const DynamicPaletteKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const DynamicPaletteKey& key) {
    return H::combine(std::move(h), key.sourceIdentity, key.width, key.height, key.format);
  }
};

struct DynamicPaletteEntry {
  gfx::TextureHandle handle;
  u32 tlutDataVersion = 0;
  uint64_t lastUsedFrame = 0;
};

struct CachedTextureEntry {
  gfx::TextureHandle handle;
  u32 texDataVersion = 0;
  u32 tlutObjId = 0;
  u32 tlutDataVersion = 0;
  uint64_t lastUsedFrame = 0;
};

struct CachedTlutTextureEntry {
  gfx::TextureHandle handle;
  u32 tlutDataVersion = 0;
};

struct TlutObjectCache {
  CachedTlutTextureEntry tlutTexture;
  absl::flat_hash_map<DynamicPaletteKey, DynamicPaletteEntry> dynamicPaletteTextures;
  absl::flat_hash_set<u32> staticTextureUsers;
  uint64_t lastUsedFrame = 0;
};

struct TextureContentKey {
  XXH128_hash_t textureHash{};
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u32 mipCount = 0;
  XXH128_hash_t tlutHash{};
  u32 tlutFormat = 0;
  u16 tlutEntries = 0;

  bool operator==(const TextureContentKey& rhs) const noexcept {
    return textureHash.low64 == rhs.textureHash.low64 && textureHash.high64 == rhs.textureHash.high64 &&
           width == rhs.width && height == rhs.height && format == rhs.format && mipCount == rhs.mipCount &&
           tlutHash.low64 == rhs.tlutHash.low64 && tlutHash.high64 == rhs.tlutHash.high64 &&
           tlutFormat == rhs.tlutFormat && tlutEntries == rhs.tlutEntries;
  }

  template <typename H>
  friend H AbslHashValue(H h, const TextureContentKey& key) {
    return H::combine(std::move(h), key.textureHash.low64, key.textureHash.high64, key.width, key.height, key.format,
                      key.mipCount, key.tlutHash.low64, key.tlutHash.high64, key.tlutFormat, key.tlutEntries);
  }
};

struct ContentCacheEntry {
  gfx::TextureHandle handle;
  uint64_t bytes = 0;
  std::list<TextureContentKey>::iterator lruIt;
};

absl::flat_hash_map<u32, CachedTextureEntry> s_textureObjectCaches;
absl::flat_hash_map<u32, TlutObjectCache> s_tlutObjectCaches;
absl::flat_hash_map<TextureContentKey, ContentCacheEntry> s_contentCache;
std::list<TextureContentKey> s_contentLru;
uint64_t s_contentCacheBytes = 0;
uint64_t s_contentCacheBudgetBytes = VitaContentCacheBudgetBytes;
uint64_t s_frameCount = 0;
uint64_t s_bindGeneration = 1;
std::atomic<uint64_t> s_pendingInvalidations = 0;
std::atomic<uint64_t> s_pendingCacheClears = 0;
TextureStats s_stats;
uint64_t s_warnedFormats = 0; // bit per GX format (0..63) already reported as unsupported

void do_clear_static_texture_cache() noexcept {
  s_textureObjectCaches.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.staticTextureUsers.clear();
  }
}

void apply_pending_invalidations() noexcept {
  const uint64_t pendingCacheClears = s_pendingCacheClears.exchange(0, std::memory_order_acq_rel);
  const uint64_t pendingInvalidations = s_pendingInvalidations.exchange(0, std::memory_order_acq_rel);
  if (pendingCacheClears != 0) {
    do_clear_static_texture_cache();
  }
  s_bindGeneration += pendingCacheClears + pendingInvalidations;
  if (s_bindGeneration == 0) {
    s_bindGeneration = 1;
  }
}

DynamicPaletteKey make_dynamic_palette_key(const GXTexObj_& obj, const GXState::CopyTextureRef& source) {
  return {
      .sourceIdentity = source.handle.get(),
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
  };
}

void clear_texture_dependency(u32 texObjId, u32 tlutObjId) {
  if (texObjId == 0 || tlutObjId == 0) {
    return;
  }
  if (auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    it->second.staticTextureUsers.erase(texObjId);
    if (!it->second.tlutTexture.handle && it->second.dynamicPaletteTextures.empty() &&
        it->second.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it);
    }
  }
}

void clear_texture_dependency(u32 texObjId, const CachedTextureEntry& entry) {
  clear_texture_dependency(texObjId, entry.tlutObjId);
}

void store_cached_texture(const GXTexObj_& obj, gfx::TextureHandle handle, u32 tlutObjId = 0,
                          u32 tlutDataVersion = 0) {
  if (obj.texObjId == 0) {
    return;
  }

  auto& entry = s_textureObjectCaches[obj.texObjId];
  if (entry.tlutObjId != tlutObjId) {
    clear_texture_dependency(obj.texObjId, entry);
  }

  entry.handle = std::move(handle);
  entry.texDataVersion = obj.texDataVersion;
  entry.tlutObjId = tlutObjId;
  entry.tlutDataVersion = tlutDataVersion;
  entry.lastUsedFrame = s_frameCount;

  if (tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlutObjId];
    cache.staticTextureUsers.insert(obj.texObjId);
    cache.lastUsedFrame = s_frameCount;
  }
}

void touch_content_cache(ContentCacheEntry& entry) {
  s_contentLru.splice(s_contentLru.begin(), s_contentLru, entry.lruIt);
  entry.lruIt = s_contentLru.begin();
}

gfx::TextureHandle find_content_texture(const TextureContentKey& key) {
  const auto it = s_contentCache.find(key);
  if (it == s_contentCache.end()) {
    return {};
  }
  touch_content_cache(it->second);
  ++s_stats.contentHits;
  return it->second.handle;
}

void publish_content_stats() noexcept {
  s_stats.contentCacheBytes = s_contentCacheBytes;
  s_stats.contentCacheEntries = s_contentCache.size();
}

void evict_content_to_budget() {
  while (s_contentCacheBytes > s_contentCacheBudgetBytes && !s_contentLru.empty()) {
    const auto cacheIt = s_contentCache.find(s_contentLru.back());
    if (cacheIt != s_contentCache.end()) {
      s_contentCacheBytes -= cacheIt->second.bytes;
      s_contentCache.erase(cacheIt);
      ++s_stats.evictions;
    }
    s_contentLru.pop_back();
  }
}

void cache_content_texture(TextureContentKey key, const gfx::TextureHandle& handle) {
  const uint64_t bytes = handle ? gfx::texture_gpu_bytes(*handle) : 0;
  if (!handle || bytes == 0 || bytes > s_contentCacheBudgetBytes) {
    return;
  }

  s_contentLru.push_front(key);
  const auto [it, inserted] = s_contentCache.emplace(
      std::move(key), ContentCacheEntry{.handle = handle, .bytes = bytes, .lruIt = s_contentLru.begin()});
  if (!inserted) {
    s_contentLru.pop_front();
    touch_content_cache(it->second);
    return;
  }
  s_contentCacheBytes += bytes;
  evict_content_to_budget();
  publish_content_stats();
}

// Upstream hashes the whole source (all mips) with XXH3-128; so do we.
TextureContentKey hash_texture_source(const GXTexObj_& obj, const GXTlutObj_* tlut) {
  const size_t textureBytes = texture::texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count());
  TextureContentKey key{
      .textureHash = XXH3_128bits(obj.data, textureBytes),
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .mipCount = obj.mip_count(),
  };
  s_stats.hashedBytes += textureBytes;
  if (tlut != nullptr) {
    const size_t tlutBytes = texture::tlut_source_size(tlut->numEntries);
    key.tlutHash = XXH3_128bits(tlut->data, tlutBytes);
    key.tlutFormat = static_cast<u32>(tlut->format);
    key.tlutEntries = tlut->numEntries;
    s_stats.hashedBytes += tlutBytes;
  }
  return key;
}

void warn_unsupported_format(u32 format) {
  const u32 bit = format & 63;
  if ((s_warnedFormats & (1ull << bit)) == 0) {
    s_warnedFormats |= 1ull << bit;
    Log.warn("texture format {:#x} cannot be sampled on the Vita backend; draws using it get no texture", format);
  }
}

// A texture the source can describe: supported format, non-zero size, guest data present.
bool texture_is_loadable(const GXTexObj_& obj) {
  if (!obj.has_data() || !gfx::is_supported_texture_format(obj.format())) {
    if (obj.has_data()) {
      warn_unsupported_format(obj.format());
    }
    return false;
  }
  return texture::texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count()) != 0;
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    cache.lastUsedFrame = s_frameCount;
    if (cache.tlutTexture.handle && cache.tlutTexture.tlutDataVersion == tlut.tlutDataVersion) {
      return cache.tlutTexture.handle;
    }
    cache.dynamicPaletteTextures.clear();
    for (const u32 texObjId : cache.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    cache.staticTextureUsers.clear();
  }

  const size_t sourceBytes = texture::tlut_source_size(tlut.numEntries);
  const auto handle = gfx::new_static_texture_2d(tlut.numEntries, 1, 1, gfx::tlut_texture_format(tlut.format),
                                                 {static_cast<const u8*>(tlut.data), sourceBytes}, true, "Loaded TLUT");
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    cache.tlutTexture.handle = handle;
    cache.tlutTexture.tlutDataVersion = tlut.tlutDataVersion;
    cache.lastUsedFrame = s_frameCount;
  }
  return handle;
}

// An EFB copy (in R4/R8 form) sampled through a TLUT. The GPU computes the palette lookup lazily, the first
// time a draw samples the texture after the copy was rewritten (gfx::prepare_texture_for_sampling).
gfx::TextureHandle resolve_dynamic_palette_texture(const GXTexObj_& obj, const GXState::CopyTextureRef& source,
                                                   const GXTlutObj_& tlut) {
  const auto tlutHandle = get_tlut_texture(tlut);
  if (!tlutHandle || !source.handle) {
    return {};
  }
  auto& tlutCache = s_tlutObjectCaches[tlut.tlutObjId];
  tlutCache.lastUsedFrame = s_frameCount;
  auto& entry = tlutCache.dynamicPaletteTextures[make_dynamic_palette_key(obj, source)];
  entry.lastUsedFrame = s_frameCount;
  if (!entry.handle) {
    // The derived texture takes the size of the source copy, not the logical size of the texture object.
    entry.handle = gfx::new_palette_derived_texture(source.handle, tlutHandle, obj.format() == GX_TF_C4,
                                                    "GX Dynamic Palette Texture");
    entry.tlutDataVersion = tlut.tlutDataVersion;
  } else if (entry.tlutDataVersion != tlut.tlutDataVersion) {
    gfx::update_palette_derivation(*entry.handle, tlutHandle);
    entry.tlutDataVersion = tlut.tlutDataVersion;
  }
  return entry.handle;
}

u32 resolved_format_for_handle(const gfx::TextureHandle& handle) {
  if (!handle) {
    return GX_TF_RGBA8;
  }
  if (handle->gxFormat != gfx::InvalidTextureFormat) {
    return handle->gxFormat;
  }
  return GX_TF_RGBA8_PC;
}

void touch_bound_texture(const GXTexObj_& obj) {
  if (auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
    it->second.lastUsedFrame = s_frameCount;
    if (it->second.tlutObjId != 0) {
      if (auto tlutIt = s_tlutObjectCaches.find(it->second.tlutObjId); tlutIt != s_tlutObjectCaches.end()) {
        tlutIt->second.lastUsedFrame = s_frameCount;
      }
    }
  }

  if (!is_palette_format(obj.format()) || obj.tlut >= g_gxState.loadedTluts.size()) {
    return;
  }
  const auto copyIt = g_gxState.copyTextures.find(obj.data);
  if (copyIt == g_gxState.copyTextures.end()) {
    return;
  }
  const auto& tlut = g_gxState.loadedTluts[obj.tlut];
  if (auto tlutIt = s_tlutObjectCaches.find(tlut.tlutObjId); tlutIt != s_tlutObjectCaches.end()) {
    tlutIt->second.lastUsedFrame = s_frameCount;
    if (auto dynamicIt = tlutIt->second.dynamicPaletteTextures.find(make_dynamic_palette_key(obj, copyIt->second));
        dynamicIt != tlutIt->second.dynamicPaletteTextures.end()) {
      dynamicIt->second.lastUsedFrame = s_frameCount;
    }
  }
}

void sweep_object_caches() {
  const uint64_t idleFrames =
      gfx::live_texture_bytes() > VitaTextureSoftLimitBytes ? VitaObjectIdleFramesUnderPressure : VitaObjectIdleFrames;
  const auto expired = [idleFrames](uint64_t lastUsedFrame) {
    return s_frameCount > lastUsedFrame && s_frameCount - lastUsedFrame > idleFrames;
  };

  for (auto it = s_textureObjectCaches.begin(); it != s_textureObjectCaches.end();) {
    if (expired(it->second.lastUsedFrame)) {
      const u32 texObjId = it->first;
      const CachedTextureEntry entry = it->second;
      s_textureObjectCaches.erase(it++);
      clear_texture_dependency(texObjId, entry);
    } else {
      ++it;
    }
  }

  for (auto cacheIt = s_tlutObjectCaches.begin(); cacheIt != s_tlutObjectCaches.end();) {
    auto& cache = cacheIt->second;
    for (auto it = cache.dynamicPaletteTextures.begin(); it != cache.dynamicPaletteTextures.end();) {
      if (expired(it->second.lastUsedFrame)) {
        cache.dynamicPaletteTextures.erase(it++);
      } else {
        ++it;
      }
    }
    if (expired(cache.lastUsedFrame) && cache.staticTextureUsers.empty() && cache.dynamicPaletteTextures.empty()) {
      s_tlutObjectCaches.erase(cacheIt++);
    } else {
      ++cacheIt;
    }
  }
}
} // namespace

const TextureStats& texture_stats() noexcept { return s_stats; }

namespace texture {
size_t texture_source_size(u32 format, u32 width, u32 height, u32 mipCount) noexcept {
  return gfx::gx_texture_source_size(format, width, height, mipCount);
}

size_t tlut_source_size(u16 numEntries) noexcept { return static_cast<size_t>(numEntries) * sizeof(u16); }

void invalidate_bindings() noexcept { s_pendingInvalidations.fetch_add(1, std::memory_order_release); }

uint64_t current_bind_generation() noexcept {
  apply_pending_invalidations();
  return s_bindGeneration;
}

// Texture replacements are not supported on the Vita; nothing is ever registered.
void invalidate_replacement(uint64_t) noexcept {}

gfx::TextureHandle resolve_static_texture(const GXTexObj_& obj) {
  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == 0) {
        entry.lastUsedFrame = s_frameCount;
        ++s_stats.objectHits;
        return entry.handle;
      }
    }
  }
  if (!texture_is_loadable(obj)) {
    return {};
  }

  const TextureContentKey key = hash_texture_source(obj, nullptr);
  gfx::TextureHandle handle = find_content_texture(key);
  if (!handle) {
    const size_t sourceBytes = texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count());
    handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),
                                        {static_cast<const uint8_t*>(obj.data), sourceBytes}, false,
                                        "GX Static Texture");
    if (!handle) {
      return {};
    }
    ++s_stats.misses;
    s_stats.uploadBytes += gfx::texture_gpu_bytes(*handle);
    cache_content_texture(key, handle);
  }
  if (!obj.no_cache()) {
    store_cached_texture(obj, handle);
  }
  return handle;
}

gfx::TextureHandle resolve_static_palette_texture(const GXTexObj_& obj, const GXTlutObj_& tlut) {
  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == tlut.tlutObjId &&
          entry.tlutDataVersion == tlut.tlutDataVersion) {
        entry.lastUsedFrame = s_frameCount;
        if (auto tlutIt = s_tlutObjectCaches.find(tlut.tlutObjId); tlutIt != s_tlutObjectCaches.end()) {
          tlutIt->second.lastUsedFrame = s_frameCount;
        }
        ++s_stats.objectHits;
        return entry.handle;
      }
    }
  }
  if (!texture_is_loadable(obj) || !is_palette_format(obj.format()) || tlut.data == nullptr || tlut.numEntries == 0) {
    return {};
  }

  const TextureContentKey key = hash_texture_source(obj, &tlut);
  gfx::TextureHandle handle = find_content_texture(key);
  if (!handle) {
    const size_t textureBytes = texture_source_size(obj.format(), obj.width(), obj.height(), obj.mip_count());
    const size_t tlutBytes = tlut_source_size(tlut.numEntries);
    // Palette lookup happens on the CPU, straight from the tiled indices into RGBA8.
    static std::vector<uint32_t> pixels;
    bool arbitraryMips = false;
    if (!gfx::decode_palette_texture(obj.format(), obj.width(), obj.height(), obj.mip_count(),
                                     static_cast<const u8*>(obj.data), textureBytes, tlut.format, tlut.numEntries,
                                     static_cast<const u8*>(tlut.data), tlutBytes, pixels, &arbitraryMips)) {
      return {};
    }
    handle = gfx::new_texture_rgba8(obj.width(), obj.height(), obj.mip_count(), pixels.data(), GX_TF_RGBA8_PC,
                                    "GX Static Palette Texture");
    if (pixels.capacity() > 4u * 1024 * 1024) {
      std::vector<uint32_t>().swap(pixels);
    }
    if (!handle) {
      return {};
    }
    handle->hasArbitraryMips = arbitraryMips;
    ++s_stats.misses;
    s_stats.uploadBytes += gfx::texture_gpu_bytes(*handle);
    cache_content_texture(key, handle);
  }
  if (!obj.no_cache() && !tlut.no_cache()) {
    store_cached_texture(obj, handle, tlut.tlutObjId, tlut.tlutDataVersion);
  }
  return handle;
}

void end_frame() noexcept {
  s_stats = {};
  publish_content_stats();
  ++s_frameCount;
  apply_pending_invalidations();
  sweep_object_caches();
}

// Drops every cached texture; the GL objects are queued for deletion and removed by
// gfx::flush_texture_deletions() / gfx::shutdown_textures() on the GL thread.
void shutdown() noexcept {
  s_textureObjectCaches.clear();
  s_tlutObjectCaches.clear();
  s_contentCache.clear();
  s_contentLru.clear();
  s_contentCacheBytes = 0;
  s_contentCacheBudgetBytes = VitaContentCacheBudgetBytes;
  s_frameCount = 0;
  s_bindGeneration = 1;
  s_pendingInvalidations.store(0, std::memory_order_release);
  s_pendingCacheClears.store(0, std::memory_order_release);
  s_stats = {};
  s_warnedFormats = 0;
}

void set_content_cache_budget_for_testing(uint64_t bytes) noexcept {
  s_contentCacheBudgetBytes = bytes;
  evict_content_to_budget();
  publish_content_stats();
}
} // namespace texture

void evict_texture_object(u32 texObjId) noexcept {
  if (const auto it = s_textureObjectCaches.find(texObjId); it != s_textureObjectCaches.end()) {
    const CachedTextureEntry entry = it->second;
    s_textureObjectCaches.erase(it);
    clear_texture_dependency(texObjId, entry);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}

void evict_tlut_object(u32 tlutObjId) noexcept {
  if (const auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    for (const u32 texObjId : it->second.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    s_tlutObjectCaches.erase(it);
  }
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
  texture::invalidate_bindings();
}

void clear_copy_texture_cache() noexcept {
  g_gxState.copyTextures.clear();
  g_gxState.copyTextureCache.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.dynamicPaletteTextures.clear();
  }
  texture::invalidate_bindings();
}

void clear_static_texture_cache() noexcept { s_pendingCacheClears.fetch_add(1, std::memory_order_release); }

void evict_copy_texture(const void* dest) noexcept {
  absl::flat_hash_set<const void*> sourceIdentities;
  if (const auto it = g_gxState.copyTextures.find(dest); it != g_gxState.copyTextures.end()) {
    if (it->second.handle) {
      sourceIdentities.insert(it->second.handle.get());
    }
    g_gxState.copyTextures.erase(it);
  }

  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      if (it->second.handle) {
        sourceIdentities.insert(it->second.handle.get());
      }
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }

  if (!sourceIdentities.empty()) {
    for (auto& [_, cache] : s_tlutObjectCaches) {
      for (auto it = cache.dynamicPaletteTextures.begin(); it != cache.dynamicPaletteTextures.end();) {
        if (sourceIdentities.contains(it->first.sourceIdentity)) {
          cache.dynamicPaletteTextures.erase(it++);
        } else {
          ++it;
        }
      }
    }
  }
  texture::invalidate_bindings();
}

void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  apply_pending_invalidations();

  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }

    GXTexObj_ obj = g_gxState.loadedTextures[i];
    auto& textureBind = g_gxState.textures[i];
    if (textureBind.generation == s_bindGeneration && obj.texObjId != 0 &&
        obj.texObjId == textureBind.texObj.texObjId && obj.texDataVersion == textureBind.texObj.texDataVersion) {
      touch_bound_texture(obj);
      continue;
    }

    gfx::TextureHandle handle;
    const auto copyIt = g_gxState.copyTextures.find(obj.data);
    const GXState::CopyTextureRef* copyRef = copyIt != g_gxState.copyTextures.end() ? &copyIt->second : nullptr;
    if (is_palette_format(obj.format())) {
      const auto tlutIdx = static_cast<size_t>(obj.tlut);
      if (tlutIdx < g_gxState.loadedTluts.size()) {
        const auto& tlut = g_gxState.loadedTluts[tlutIdx];
        if (tlut.data != nullptr) {
          if (copyRef != nullptr) {
            handle = resolve_dynamic_palette_texture(obj, *copyRef, tlut);
          } else if (obj.has_data()) {
            handle = texture::resolve_static_palette_texture(obj, tlut);
          }
        }
      }
    } else if (copyRef != nullptr) {
      handle = copyRef->handle;
    } else if (obj.has_data()) {
      handle = texture::resolve_static_texture(obj);
    }

    obj.mFormat = resolved_format_for_handle(handle);
    textureBind = gfx::TextureBind{obj, std::move(handle), s_bindGeneration};
  }
}
} // namespace aurora::gx
