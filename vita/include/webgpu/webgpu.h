#ifndef VITA_WEBGPU_H
#define VITA_WEBGPU_H
/*
 * Vita shim for <webgpu/webgpu.h>.
 *
 * Dusklight's Aurora renderer uses Chromium Dawn/WebGPU (D3D12/Vulkan/Metal).
 * The Vita port replaces that with its own GX/vitaGL backend, so no real WebGPU
 * exists here. This header keeps game-compilation happy when the *mods service*
 * header (sdk/include/mods/svc/gfx.h) is included by game code
 * (src/m_Do/m_Do_graphic.cpp via dusk/gfx.hpp): only the opaque handle types and
 * the enum constants those structs reference are needed for parsing.
 * The actual mods/svc/gfx.cpp and mods/svc/camera.cpp implementations are NOT
 * compiled on Vita (see CMakeLists.txt TPV_DUSK_EXCLUDE).
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque handles (borrowed throughout the mods service header) ---- */
typedef struct WGPUInstanceImpl*       WGPUInstance;
typedef struct WGPUAdapterImpl*        WGPUAdapter;
typedef struct WGPUDeviceImpl*         WGPUDevice;
typedef struct WGPUQueueImpl*          WGPUQueue;
typedef struct WGPUSurfaceImpl*        WGPUSurface;
typedef struct WGPUTextureImpl*        WGPUTexture;
typedef struct WGPUTextureViewImpl*    WGPUTextureView;
typedef struct WGPURenderPassEncoderImpl* WGPURenderPassEncoder;
typedef struct WGPUCommandEncoderImpl* WGPUCommandEncoder;
typedef struct WGPUBufferImpl*         WGPUBuffer;

/* ---- Enums referenced by mods/svc/gfx.h ---- */
typedef enum WGPUTextureFormat {
    WGPUTextureFormat_Undefined = 0,
    WGPUTextureFormat_RGBA8Unorm = 11,
    WGPUTextureFormat_BGRA8Unorm = 12,
    WGPUTextureFormat_Depth32Float = 63,
} WGPUTextureFormat;

typedef enum WGPUTextureUsage {
    WGPUTextureUsage_None                = 0x0,
    WGPUTextureUsage_CopySrc             = 0x1,
    WGPUTextureUsage_CopyDst             = 0x2,
    WGPUTextureUsage_TextureBinding      = 0x4,
    WGPUTextureUsage_StorageBinding      = 0x8,
    WGPUTextureUsage_RenderAttachment    = 0x10,
    WGPUTextureUsage_StorageAttachment   = 0x20,
} WGPUTextureUsage;

typedef enum WGPUCompositeAlphaMode {
    WGPUCompositeAlphaMode_Auto = 0,
} WGPUCompositeAlphaMode;

typedef enum WGPUColorWriteMask {
    WGPUColorWriteMask_None  = 0x0,
    WGPUColorWriteMask_Red   = 0x1,
    WGPUColorWriteMask_Green = 0x2,
    WGPUColorWriteMask_Blue  = 0x4,
    WGPUColorWriteMask_Alpha = 0x8,
    WGPUColorWriteMask_All   = 0xF,
} WGPUColorWriteMask;

typedef struct WGPUBlendState {
    /* blend component; opaque for parsing only */
    uint32_t pad[8];
} WGPUBlendState;

typedef struct WGPUColorTargetState {
    WGPUTextureFormat format;     /* WGPUTextureFormat */
    const WGPUBlendState* blend;  /* const WGPUBlendState* */
    WGPUColorWriteMask writeMask; /* WGPUColorWriteMaskFlags */
} WGPUColorTargetState;

#define WGPU_COLOR_TARGET_STATE_INIT { WGPUTextureFormat_Undefined, NULL, WGPUColorWriteMask_All }

#ifdef __cplusplus
}
#endif

#endif /* VITA_WEBGPU_H */
