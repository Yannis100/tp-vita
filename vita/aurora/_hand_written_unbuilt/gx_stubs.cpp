/**
 * Vita port: placeholder GX API stubs.
 *
 * Aurora's real GX implementation (extern/aurora/lib/gx/*) is bound to the
 * WebGPU/Dawn backend, which does not exist on Vita. These stubs provide every
 * GX entry point the game calls so the native recompile links. They are no-ops
 * for now; the placeholder renderer (vita/aurora/backend.cpp) reports
 * BACKEND_NULL, so the game boots into the null-backend UI loop rather than
 * drawing. The real vitaGL GX backend replaces these functions one subsystem at
 * a time (state tracking first, then the command processor -> TEV shader path).
 *
 * Signatures come straight from the aurora dolphin GX headers, so no downstream
 * code needs to change.
 */

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXBump.h>
#include <dolphin/gx/GXCpu2Efb.h>
#include <dolphin/gx/GXCull.h>
#include <dolphin/gx/GXDispList.h>
#include <dolphin/gx/GXDraw.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/gx/GXFifo.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXGeometry.h>
#include <dolphin/gx/GXGet.h>
#include <dolphin/gx/GXLighting.h>
#include <dolphin/gx/GXManage.h>
#include <dolphin/gx/GXPerf.h>
#include <dolphin/gx/GXPixel.h>
#include <dolphin/gx/GXStruct.h>
#include <dolphin/gx/GXTev.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/gx/GXTransform.h>
#include <dolphin/gx/GXVert.h>
#include <dolphin/mtx.h>
#include <dolphin/types.h>

#include <cstring>

extern "C" {

// ---------------------------------------------------------------------------
// GXManage.h
// ---------------------------------------------------------------------------
GXFifoObj* GXInit(void* base, u32 size) {
    (void)base;
    (void)size;
    return nullptr;
}
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) { return cb; }
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) { return cb; }
void GXDrawDone(void) {}
void GXSetDrawDone(void) {}
void GXFlush(void) {}
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}
void GXSetMisc(GXMiscToken token, u32 val) {
    (void)token;
    (void)val;
}
void GXAbortFrame(void) {}

// ---------------------------------------------------------------------------
// GXGet.h
// ---------------------------------------------------------------------------
void GXGetVtxDesc(GXAttr attr, GXAttrType* type) {
    (void)attr;
    if (type != nullptr) {
        *type = GX_NONE;
    }
}
void GXGetVtxDescv(GXVtxDescList* vcd) {
    if (vcd != nullptr) {
        vcd->attr = GX_VA_NULL;
        vcd->type = GX_NONE;
    }
}
void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* vat) {
    (void)fmt;
    if (vat != nullptr) {
        vat->attr = GX_VA_NULL;
    }
}
void GXGetLineWidth(u8* width, GXTexOffset* texOffsets) {
    if (width != nullptr) {
        *width = 6;
    }
    if (texOffsets != nullptr) {
        *texOffsets = GX_TO_ZERO;
    }
}
void GXGetPointSize(u8* pointSize, GXTexOffset* texOffsets) {
    if (pointSize != nullptr) {
        *pointSize = 6;
    }
    if (texOffsets != nullptr) {
        *texOffsets = GX_TO_ZERO;
    }
}
void GXGetCullMode(GXCullMode* mode) {
    if (mode != nullptr) {
        *mode = GX_CULL_NONE;
    }
}
GXBool GXGetTexObjMipMap(const GXTexObj* tex_obj) { (void)tex_obj; return GX_FALSE; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* tex_obj) { (void)tex_obj; return GX_TF_RGBA8; }
u16 GXGetTexObjHeight(const GXTexObj* tex_obj) { (void)tex_obj; return 0; }
u16 GXGetTexObjWidth(const GXTexObj* tex_obj) { (void)tex_obj; return 0; }
GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* tex_obj) { (void)tex_obj; return GX_CLAMP; }
GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* tex_obj) { (void)tex_obj; return GX_CLAMP; }
void* GXGetTexObjData(const GXTexObj* tex_obj) { (void)tex_obj; return nullptr; }
void GXGetProjectionv(f32* p) {
    if (p != nullptr) {
        std::memset(p, 0, 4 * sizeof(f32));
    }
}
void GXGetLightPos(const GXLightObj* lt_obj, f32* x, f32* y, f32* z) {
    (void)lt_obj;
    if (x != nullptr) *x = 0.f;
    if (y != nullptr) *y = 0.f;
    if (z != nullptr) *z = 0.f;
}
void GXGetLightColor(const GXLightObj* lt_obj, GXColor* color) {
    (void)lt_obj;
    if (color != nullptr) {
        *color = GXColor{0xff, 0xff, 0xff, 0xff};
    }
}
void GXGetVtxAttrFmt(GXVtxFmt idx, GXAttr attr, GXCompCnt* compCnt, GXCompType* compType, u8* shift) {
    (void)idx;
    (void)attr;
    if (compCnt != nullptr) *compCnt = GX_POS_XYZ;
    if (compType != nullptr) *compType = GX_F32;
    if (shift != nullptr) *shift = 0;
}
u32 GXGetTexObjTlut(const GXTexObj* tex_obj) { (void)tex_obj; return 0; }
void GXGetViewportv(f32* vp) {
    if (vp != nullptr) {
        vp[0] = 0.f;
        vp[1] = 0.f;
        vp[2] = 640.f;
        vp[3] = 480.f;
        vp[4] = 0.f;
        vp[5] = 1.f;
    }
}
void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (left != nullptr) *left = 0;
    if (top != nullptr) *top = 0;
    if (wd != nullptr) *wd = 640;
    if (ht != nullptr) *ht = 480;
}

// ---------------------------------------------------------------------------
// GXGeometry.h
// ---------------------------------------------------------------------------
void GXSetVtxDesc(GXAttr attr, GXAttrType type) { (void)attr; (void)type; }
void GXSetVtxDescv(GXVtxDescList* list) { (void)list; }
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) { (void)vtxfmt; (void)list; }
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    (void)vtxfmt; (void)attr; (void)cnt; (void)type; (void)frac;
}
void GXSetNumTexGens(u8 nTexGens) { (void)nTexGens; }
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) { (void)type; (void)vtxfmt; (void)nverts; }
void GXBeginIndexed(GXVtxFmt vtxfmt, u16 nverts, const u16* indices, u32 nindices) {
    (void)vtxfmt; (void)nverts; (void)indices; (void)nindices;
}
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx,
    GXBool normalize, GXPostTexGen post) {
    (void)dst_coord; (void)func; (void)src_param; (void)mtx; (void)normalize; (void)post;
}
void GXSetLineWidth(u8 width, GXTexOffset texOffsets) { (void)width; (void)texOffsets; }
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) { (void)pointSize; (void)texOffsets; }
void GXEnableTexOffsets(GXTexCoordID coord, GXBool enable, GXBool offset_mtx) {
    (void)coord; (void)enable; (void)offset_mtx;
}
void GXSetNumChanCtrl(GXChannelID channel) { (void)channel; }
void GXSetChanCtrl(GXChannelID channel, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
    GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    (void)channel; (void)enable; (void)amb_src; (void)mat_src; (void)light_mask; (void)diff_fn; (void)attn_fn;
}
void GXSetChanAmbColor(GXChannelID channel, GXColor amb_color) { (void)channel; (void)amb_color; }
void GXSetChanMatColor(GXChannelID channel, GXColor mat_color) { (void)channel; (void)mat_color; }
void GXSetNumChans(u8 nChans) { (void)nChans; }
void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride) { (void)attr; (void)base_ptr; (void)stride; }
void GXSetCoPlanar(GXBool enable) { (void)enable; }

// ---------------------------------------------------------------------------
// GXVert.h / GXDraw.h
// ---------------------------------------------------------------------------
void GXPosition3f32(f32 x, f32 y, f32 z) { (void)x; (void)y; (void)z; }
void GXPosition3u16(u16 x, u16 y, u16 z) { (void)x; (void)y; (void)z; }
void GXPosition3s16(s16 x, s16 y, s16 z) { (void)x; (void)y; (void)z; }
void GXPosition3u8(u8 x, u8 y, u8 z) { (void)x; (void)y; (void)z; }
void GXPosition3s8(s8 x, s8 y, s8 z) { (void)x; (void)y; (void)z; }
void GXPosition2f32(f32 x, f32 y) { (void)x; (void)y; }
void GXPosition2u16(u16 x, u16 y) { (void)x; (void)y; }
void GXPosition2s16(s16 x, s16 y) { (void)x; (void)y; }
void GXPosition2u8(u8 x, u8 y) { (void)x; (void)y; }
void GXPosition2s8(s8 x, s8 y) { (void)x; (void)y; }
void GXPosition1x16(u16 x) { (void)x; }
void GXPosition1x8(u8 x) { (void)x; }
void GXNormal3f32(f32 x, f32 y, f32 z) { (void)x; (void)y; (void)z; }
void GXNormal3s16(s16 x, s16 y, s16 z) { (void)x; (void)y; (void)z; }
void GXNormal3s8(s8 x, s8 y, s8 z) { (void)x; (void)y; (void)z; }
void GXNormal1x16(u16 x) { (void)x; }
void GXNormal1x8(u8 x) { (void)x; }
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { (void)r; (void)g; (void)b; (void)a; }
void GXColor1u32(u32 c) { (void)c; }
void GXColor1x16(u16 c) { (void)c; }
void GXColor1x8(u8 c) { (void)c; }
void GXColor3u8(u8 r, u8 g, u8 b) { (void)r; (void)g; (void)b; }
void GXColor3f32(f32 r, f32 g, f32 b) { (void)r; (void)g; (void)b; }
void GXTexCoord2f32(f32 s, f32 t) { (void)s; (void)t; }
void GXTexCoord2u16(u16 s, u16 t) { (void)s; (void)t; }
void GXTexCoord2s16(s16 s, s16 t) { (void)s; (void)t; }
void GXTexCoord2u8(u8 s, u8 t) { (void)s; (void)t; }
void GXTexCoord2s8(s8 s, s8 t) { (void)s; (void)t; }
void GXTexCoord1x16(u16 s) { (void)s; }
void GXTexCoord1x8(u8 s) { (void)s; }
void GXTexCoord2f32_s(s s_, f32 t) { (void)s_; (void)t; }
void GXEnd(void) {}
void GXCallDisplayList(const void* list, u32 nbytes) { (void)list; (void)nbytes; }
void GXDrawSphere(u8 major, u8 minor) { (void)major; (void)minor; }
void GXDrawCylinder(u8 sections, u8 major, u8 minor) { (void)sections; (void)major; (void)minor; }
void GXDrawCube(void) {}
void GXDrawDodeca(void) {}
void GXDrawOctahedron(void) {}
void GXDrawTetrahedron(void) {}
void GXDrawCylinder8(u8 n) { (void)n; }

// ---------------------------------------------------------------------------
// GXTransform.h
// ---------------------------------------------------------------------------
void GXProject(f32 x, f32 y, f32 z, const Mtx inMtx, const f32* pm, f32* outx, f32* outy, f32* outz) {
    (void)x; (void)y; (void)z; (void)inMtx; (void)pm;
    if (outx != nullptr) *outx = 0.f;
    if (outy != nullptr) *outy = 0.f;
    if (outz != nullptr) *outz = 0.f;
}
void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) { (void)mtx; (void)type; }
void GXSetProjectionv(const f32* mtx) { (void)mtx; }
void GXSetProjectionFull(const void* mtx) { (void)mtx; }
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    (void)left; (void)top; (void)wd; (void)ht; (void)nearz; (void)farz;
}
void GXSetViewportv(const f32* viewport) { (void)viewport; }
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    (void)left; (void)top; (void)wd; (void)ht; (void)nearz; (void)farz;
}
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetScissorBoxOffset(s32 x_off, s32 y_off) { (void)x_off; (void)y_off; }
void GXSetClipMode(GXClipMode mode) { (void)mode; }

// ---------------------------------------------------------------------------
// GXLighting.h
// ---------------------------------------------------------------------------
void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    (void)lt_obj; (void)a0; (void)a1; (void)a2; (void)k0; (void)k1; (void)k2;
}
void GXInitLightColor(GXLightObj* lt_obj, GXColor color) { (void)lt_obj; (void)color; }
void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) { (void)lt_obj; (void)x; (void)y; (void)z; }
void GXInitLightPosv(GXLightObj* lt_obj, const Vec* pos) { (void)lt_obj; (void)pos; }
void GXInitLightDir(GXLightObj* lt_obj, f32 x, f32 y, f32 z) { (void)lt_obj; (void)x; (void)y; (void)z; }
void GXInitLightDirv(GXLightObj* lt_obj, const Vec* dir) { (void)lt_obj; (void)dir; }
void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_brightness, GXLightDistAttnFn dist_fn) {
    (void)lt_obj; (void)ref_dist; (void)ref_brightness; (void)dist_fn;
}
void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_fn) { (void)lt_obj; (void)cutoff; (void)spot_fn; }
void GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light) { (void)lt_obj; (void)light; }
void GXLoadLightObjIndx(u32 ltobj_idx, GXLightID light) { (void)ltobj_idx; (void)light; }
void GXLoadLightObj(GXLightObj* lt_obj, GXLightID light) { (void)lt_obj; (void)light; }
void GXSetLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    (void)lt_obj; (void)a0; (void)a1; (void)a2; (void)k0; (void)k1; (void)k2;
}

// ---------------------------------------------------------------------------
// GXTexture.h
// ---------------------------------------------------------------------------
void GXInitTexObj(GXTexObj* obj, void* dataptr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
    GXTexWrapMode wrap_t, GXBool mipmap) {
    (void)obj; (void)dataptr; (void)width; (void)height; (void)format; (void)wrap_s; (void)wrap_t; (void)mipmap;
}
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod,
    f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso) {
    (void)obj; (void)min_filt; (void)mag_filt; (void)min_lod; (void)max_lod; (void)lod_bias;
    (void)bias_clamp; (void)do_edge_lod; (void)max_aniso;
}
void GXInitTexObjCI(GXTexObj* obj, void* dataptr, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrap_s,
    GXTexWrapMode wrap_t, GXBool mipmap, u32 tlut_name) {
    (void)obj; (void)dataptr; (void)width; (void)height; (void)format; (void)wrap_s; (void)wrap_t;
    (void)mipmap; (void)tlut_name;
}
void GXInitTexObjTlut(GXTexObj* obj, u32 tlut_name) { (void)obj; (void)tlut_name; }
void GXInitTexObjData(GXTexObj* obj, void* dataptr) { (void)obj; (void)dataptr; }
void GXInitTlutObj(GXTlutObj* obj, void* data, GXTlutFmt format, u16 nentries) {
    (void)obj; (void)data; (void)format; (void)nentries;
}
void GXInitTlutObjData(GXTlutObj* obj, void* data) { (void)obj; (void)data; }
void GXLoadTexObj(GXTexObj* obj, GXTexMapID mapid) { (void)obj; (void)mapid; }
void GXLoadTexObjMtx(GXTexObj* obj, GXTexMapID mapid, u32 texmtx) { (void)obj; (void)mapid; (void)texmtx; }
void GXLoadTlut(GXTlutObj* obj, u32 tlut_name) { (void)obj; (void)tlut_name; }
void GXLoadTlutRaw(u32 tlut_name, void* tlut, u32 size) { (void)tlut_name; (void)tlut; (void)size; }
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod, GXBool use_best_dim) {
    (void)width; (void)height; (void)format; (void)mipmap; (void)max_lod; (void)use_best_dim; return 0;
}
void GXInvalidateTexAll(void) {}
void GXInvalidateVtxCache(void) {}
void GXInitTexCacheRegion(GXTexRegion* region, void* memBase, u32 size, u32 tmem_addr, GXBool is_cached,
    GXTexCacheSize cache_size, GXBool is_dithered, u32 palette_base) {
    (void)region; (void)memBase; (void)size; (void)tmem_addr; (void)is_cached; (void)cache_size;
    (void)is_dithered; (void)palette_base;
}
void GXDestroyTexObj(GXTexObj* obj) { (void)obj; }
void GXDestroyTlutObj(GXTlutObj* obj) { (void)obj; }

// ---------------------------------------------------------------------------
// GXFifo.h
// ---------------------------------------------------------------------------
void GXSetCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) { (void)fifo; (void)base; (void)size; }
void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) { (void)fifo; (void)readPtr; (void)writePtr; }
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    (void)overhi; (void)underlow; (void)readIdle; (void)cmdIdle; (void)brkpt;
}
GXFifoObj* GXGetCPUFifo(void) { return nullptr; }
void* GXGetFifoBase(void) { return nullptr; }
u32 GXGetFifoSize(void) { return 0; }
u32 GXGetCurrentGXThread(void) { return 0; }
void GXSetCurrentGXThread(void) {}
void GXSaveCPUFifo(GXFifoObj* fifo) { (void)fifo; }
u32 GXGetFifoCount(void) { return 0; }
u8 GXGetFifoWrap(void) { return 0; }
void GXSetGXThread(u8 b) { (void)b; }

// ---------------------------------------------------------------------------
// GXPixel.h / GXFrameBuffer.h / GXPerf.h
// ---------------------------------------------------------------------------
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    (void)type; (void)src_factor; (void)dst_factor; (void)op;
}
void GXSetColorUpdate(GXBool update_enable) { (void)update_enable; }
void GXSetAlphaUpdate(GXBool update_enable) { (void)update_enable; }
void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    (void)compare_enable; (void)func; (void)update_enable;
}
void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }
void GXSetZTexture(GXZTexOp op, GXTexFmt format, u32 bias) { (void)op; (void)format; (void)bias; }
void GXSetDither(GXBool dither) { (void)dither; }
void GXSetDstAlpha(GXBool enable, u8 alpha) { (void)enable; (void)alpha; }
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    (void)comp0; (void)ref0; (void)op; (void)comp1; (void)ref1;
}
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) { (void)pix_fmt; (void)z_fmt; }
void GXSetCopyClear(GXColor clear_clr, u32 clear_z) { (void)clear_clr; (void)clear_z; }
void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetDispCopyDst(u16 wd, u16 ht) { (void)wd; (void)ht; }
void GXSetCopyClamp(GXFBClamp clamp) { (void)clamp; }
void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
    (void)aa; (void)sample_pattern; (void)vf; (void)vfilter;
}
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) { (void)wd; (void)ht; (void)fmt; (void)mipmap; }
void GXCopyTex(void* dest, GXBool clear) { (void)dest; (void)clear; }
void GXCopyDisp(void* dest, GXBool clear) { (void)dest; (void)clear; }
void GXCreateFrameBuffer(void) {}
void GXRestoreFrameBuffer(void) {}
void GXDestroyCopyTex(void) {}
void GXSetScissorRenderMode(void) {}
void GXPeekZ(u16 x, u16 y, u32* z) { (void)x; (void)y; if (z != nullptr) *z = 0; }
u32 GXGetTexObjLoadCount(GXTexObj* obj) { (void)obj; return 0; }
u32 GXGetNumXfbLines(void) { return 480; }
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { (void)efbHeight; (void)xfbHeight; return 1.f; }
void GXReadXfRasMetric(u32* xfWaitIn, u32* xfWaitOut, u32* rasBusy, u32* clocks) {
    (void)xfWaitIn; (void)xfWaitOut; (void)rasBusy; (void)clocks;
}

// ---------------------------------------------------------------------------
// GXTev.h / GXBump.h
// ---------------------------------------------------------------------------
void GXSetNumTevStages(u8 nStages) { (void)nStages; }
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    (void)stage; (void)coord; (void)map; (void)color;
}
void GXSetTevInvalid(GXTevStageID stage) { (void)stage; }
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    (void)stage; (void)a; (void)b; (void)c; (void)d;
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    (void)stage; (void)a; (void)b; (void)c; (void)d;
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg) {
    (void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)out_reg;
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg) {
    (void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)out_reg;
}
void GXSetTevColor(GXTevRegID reg, GXColor color) { (void)reg; (void)color; }
void GXSetTevColorS10(GXTevRegID reg, GXColorS10 color) { (void)reg; (void)color; }
void GXSetTevKColor(GXTevKColorID sel, GXColor color) { (void)sel; (void)color; }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { (void)stage; (void)sel; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { (void)stage; (void)sel; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    (void)stage; (void)ras_sel; (void)tex_sel;
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTexColorChannel red, GXTexColorChannel green,
    GXTexColorChannel blue, GXTexColorChannel alpha) {
    (void)table; (void)red; (void)green; (void)blue; (void)alpha;
}
void GXSetTevDirect(GXTevStageID tev_stage) { (void)tev_stage; }
void GXSetTevIndTile(GXTevStageID tevStage, GXIndTexStageID indStage, u16 tileSizeS, u16 tileSizeT,
    u16 tileSpacingS, u16 tileSpacingT, GXIndTexFormat format, GXIndTexBiasSel biasSel, GXIndTexAlphaSel alphaSel) {
    (void)tevStage; (void)indStage; (void)tileSizeS; (void)tileSizeT; (void)tileSpacingS; (void)tileSpacingT;
    (void)format; (void)biasSel; (void)alphaSel;
}
void GXSetNumIndStages(u8 nIndStages) { (void)nIndStages; }
void GXSetIndTexMtx(GXIndTexMtxID mtx_sel, const void* offset, s8 scale_exp) { (void)mtx_sel; (void)offset; (void)scale_exp; }
void GXSetIndTexMtx(GXIndTexMtxID mtx_sel, f32 offset[2][3], s8 scale_exp) { (void)mtx_sel; (void)offset; (void)scale_exp; }
void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    (void)ind_stage; (void)tex_coord; (void)tex_map;
}
void GXSetIndTexCoordScale(GXIndTexStageID indStage, GXIndTexScale scaleS, GXIndTexScale scaleT) {
    (void)indStage; (void)scaleS; (void)scaleT;
}
void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
    GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,
    GXBool add_prev, GXIndTexAlphaSel alpha_sel, GXIndTevAlphaMode alpha_in, GXIndTexScale scale_s, GXIndTexScale scale_t) {
    (void)tev_stage; (void)ind_stage; (void)format; (void)bias_sel; (void)matrix_sel; (void)wrap_s;
    (void)wrap_t; (void)add_prev; (void)alpha_sel; (void)alpha_in; (void)scale_s; (void)scale_t;
}
void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXBool signed_offsets,
    GXBool replace_mode, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,
    GXBool add_prev, GXIndTexAlphaSel alpha_sel, GXIndTevAlphaMode alpha_in, GXIndTexScale scale_s, GXIndTexScale scale_t) {
    (void)tev_stage; (void)ind_stage; (void)signed_offsets; (void)replace_mode; (void)matrix_sel;
    (void)wrap_s; (void)wrap_t; (void)add_prev; (void)alpha_sel; (void)alpha_in; (void)scale_s; (void)scale_t;
}
void GXSetTevIndRepeat(GXTevStageID tev_stage) { (void)tev_stage; }
void GXSetTevIndBumpXYZ(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexMtxID matrix_sel) {
    (void)tev_stage; (void)ind_stage; (void)matrix_sel;
}
void GXSetTevIndBumpST(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexMtxID matrix_sel) {
    (void)tev_stage; (void)ind_stage; (void)matrix_sel;
}

// ---------------------------------------------------------------------------
// GXExtra / GXCull / GXDebug
// ---------------------------------------------------------------------------
void GXColor4f32(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
GXColor GXColorScale(GXColor src, float scale) { (void)src; (void)scale; return GXColor{255, 255, 255, 255}; }
GXColor GXColorSetAlpha(GXColor src, u8 alpha) { (void)src; (void)alpha; return GXColor{255, 255, 255, 255}; }
GXColor GXColorMultiply(GXColor src, GXColor dst) { (void)src; (void)dst; return GXColor{255, 255, 255, 255}; }
GXColor GXColorAdd(GXColor a, GXColor b) { (void)a; (void)b; return GXColor{255, 255, 255, 255}; }
GXColor GXColorSub(GXColor a, GXColor b) { (void)a; (void)b; return GXColor{255, 255, 255, 255}; }
void GXSetScissorRender1(void) {}
void GXSetCullMode(GXCullMode mode) { (void)mode; }
void GXClearVtxDesc(void) {}
void GXPushDebugGroup(const char* label) { (void)label; }
void GXPopDebugGroup(void) {}
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    (void)type; (void)startz; (void)endz; (void)nearz; (void)farz; (void)color;
}
void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) { (void)enable; (void)center; (void)table; }

// ---------------------------------------------------------------------------
// GXCpu2Efb.h
// ---------------------------------------------------------------------------
GXTexObj* GXLoadTexObjCpu(GXTexObj* obj, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s,
    GXTexWrapMode wrap_t, GXBool mipmap) {
    (void)obj; (void)width; (void)height; (void)format; (void)wrap_s; (void)wrap_t; (void)mipmap; return obj;
}
void GXLoadTexObjCpuMips(GXTexObj* obj, GXTexMapID mapid, u16 width, u16 height, GXTexFmt format,
    u8 max_lod, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, GXBool mipmap) {
    (void)obj; (void)mapid; (void)width; (void)height; (void)format; (void)max_lod; (void)wrap_s; (void)wrap_t; (void)mipmap;
}

// ---------------------------------------------------------------------------
// GXNtsc480Int etc (GXEnum / vi helpers)
// ---------------------------------------------------------------------------
GXRenderModeObj GXNtsc480Int = {0};
GXRenderModeObj GXNtsc480IntDf = {0};
GXRenderModeObj GXNtsc480Prog = {0};
GXRenderModeObj GXNtsc480ProgSoft = {0};

}  // extern "C"
