#ifndef _DOLPHIN_MTX
#define _DOLPHIN_MTX
/*
 * Vita-side declaration layer for TP decomp game logic (Tier-1 skeleton).
 * Declares the matrix/vector/quaternion API surface. On ARM we compile the
 * portable C implementations (C_MTX*, JMA*) — no PPC/PS asm.
 */

#include <dolphin/mtx/GeoTypes.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTX_PTR_OFFSET 3
#define MTX44_PTR_OFFSET 4

#define MTXDegToRad(a) ((a)*0.01745329252f)
#define MTXRadToDeg(a) ((a)*57.29577951f)
#define MTXRowCol(m, r, c) ((m)[(r)][(c)])

typedef struct {
  u32 numMtx;
  MtxPtr stackBase;
  MtxPtr stackPtr;
} MTXStack, MtxStack, *MtxStackPtr;

void C_MTXIdentity(Mtx m);
void C_MTXCopy(const Mtx src, Mtx dst);
void C_MTXConcat(const Mtx a, const Mtx b, Mtx ab);
void C_MTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count);
void C_MTXTranspose(const Mtx src, Mtx xPose);
u32 C_MTXInverse(const Mtx src, Mtx inv);
u32 C_MTXInvXpose(const Mtx src, Mtx invX);

void C_MTXMultVec(const Mtx m, const Vec* src, Vec* dst);
void C_MTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void C_MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst);
void C_MTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void C_MTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count);
void C_MTXMultS16VecArray(const Mtx m, const S16Vec* srcBase, Vec* dstBase, u32 count);
void C_MTXROMultS16VecArray(const ROMtx m, const S16Vec* srcBase, Vec* dstBase, u32 count);

void C_MTXQuat(Mtx m, const Quaternion* q);
void C_MTXReflect(Mtx m, const Vec* p, const Vec* n);
void C_MTXTrans(Mtx m, f32 xT, f32 yT, f32 zT);
void C_MTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT);
void C_MTXScale(Mtx m, f32 xS, f32 yS, f32 zS);
void C_MTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS);
void C_MTXRotRad(Mtx m, char axis, f32 rad);
void C_MTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA);
void C_MTXRotAxisRad(Mtx m, const Vec* axis, f32 rad);

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target);
void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f);
void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
void C_MTXLighting(Mtx44 m, const Vec* pos, const Vec* up, const Vec* left);

void C_VECAdd(const Vec* a, const Vec* b, Vec* ab);
void C_VECSubtract(const Vec* a, const Vec* b, Vec* ab);
void C_VECCross(const Vec* a, const Vec* b, Vec* ab);
f32 C_VECDotProduct(const Vec* a, const Vec* b);
f32 C_VECSquareMag(const Vec* a);
f32 C_VECMag(const Vec* a);
void C_VECNormalize(const Vec* src, Vec* dst);
f32 C_VECDistance(const Vec* a, const Vec* b);
void C_VECScale(const Vec* src, Vec* dst, f32 scale);
void C_VECAddScaled(const Vec* a, const Vec* b, Vec* ab, f32 scale);

void C_QUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* r);
void C_QUATDivide(const Quaternion* p, const Quaternion* q, Quaternion* r);
void C_QUATScale(const Quaternion* p, Quaternion* r, f32 scale);
void C_QUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r);
void C_QUATSubtract(const Quaternion* p, const Quaternion* q, Quaternion* r);
void C_QUATDot(const Quaternion* p, const Quaternion* q, f32* dot);
void C_QUATNormalize(const Quaternion* p, Quaternion* r);
void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t);
void C_QUATExp(const Quaternion* p, Quaternion* r);
void C_QUATLog(const Quaternion* p, Quaternion* r);
void C_QUATAxisRad(Quaternion* r, const Vec* axis, f32 rad);
void C_QUATMatrix(const Quaternion* q, Mtx m);
void C_QUATRotVec(const Quaternion* q, const Vec* src, Vec* dst);

/* Aliases used by the game code when it calls MTX* / PSMTX* directly. */
#define MTXIdentity   C_MTXIdentity
#define MTXCopy       C_MTXCopy
#define MTXConcat     C_MTXConcat
#define MTXConcatArray C_MTXConcatArray
#define MTXTranspose  C_MTXTranspose
#define MTXInverse    C_MTXInverse
#define MTXInvXpose   C_MTXInvXpose
#define MTXMultVec    C_MTXMultVec
#define MTXMultVecArray C_MTXMultVecArray
#define MTXMultVecSR  C_MTXMultVecSR
#define MTXMultVecArraySR C_MTXMultVecArraySR
#define MTXMultS16VecArray C_MTXMultS16VecArray
#define MTXROMultVecArray C_MTXROMultVecArray
#define MTXROMultS16VecArray C_MTXROMultS16VecArray

#define PSMTXIdentity   C_MTXIdentity
#define PSMTXCopy       C_MTXCopy
#define PSMTXConcat     C_MTXConcat
#define PSMTXConcatArray C_MTXConcatArray
#define PSMTXTranspose  C_MTXTranspose
#define PSMTXInverse    C_MTXInverse
#define PSMTXInvXpose   C_MTXInvXpose
#define PSMTXMultVec    C_MTXMultVec
#define PSMTXMultVecArray C_MTXMultVecArray
#define PSMTXMultVecSR  C_MTXMultVecSR
#define PSMTXMultVecArraySR C_MTXMultVecArraySR
#define PSMTXMultS16VecArray C_MTXMultS16VecArray
#define PSMTXROMultVecArray C_MTXROMultVecArray
#define PSMTXROMultS16VecArray C_MTXROMultS16VecArray
#define PSMTXReorder    C_MTXReorder

#define MTXQuat        C_MTXQuat
#define MTXReflect     C_MTXReflect
#define MTXTrans       C_MTXTrans
#define MTXTransApply  C_MTXTransApply
#define MTXScale       C_MTXScale
#define MTXScaleApply  C_MTXScaleApply
#define MTXRotRad      C_MTXRotRad
#define MTXRotTrig     C_MTXRotTrig
#define MTXRotAxisRad  C_MTXRotAxisRad

#define MTXLookAt      C_MTXLookAt
#define MTXFrustum     C_MTXFrustum
#define MTXPerspective C_MTXPerspective
#define MTXOrtho       C_MTXOrtho
#define MTXLighting    C_MTXLighting

#define VECAdd       C_VECAdd
#define VECSubtract  C_VECSubtract
#define VECCross     C_VECCross
#define VECDotProduct C_VECDotProduct
#define VECSquareMag C_VECSquareMag
#define VECMag       C_VECMag
#define VECNormalize C_VECNormalize
#define VECDistance  C_VECDistance
#define VECScale     C_VECScale
#define VECAddScaled C_VECAddScaled

#define QUATMultiply  C_QUATMultiply
#define QUATDivide    C_QUATDivide
#define QUATScale     C_QUATScale
#define QUATAdd       C_QUATAdd
#define QUATSubtract  C_QUATSubtract
#define QUATDot       C_QUATDot
#define QUATNormalize C_QUATNormalize
#define QUATSlerp     C_QUATSlerp
#define QUATExp       C_QUATExp
#define QUATLog       C_QUATLog
#define QUATAxisRad   C_QUATAxisRad
#define QUATMatrix    C_QUATMatrix
#define QUATRotVec    C_QUATRotVec

#ifdef __cplusplus
}
#endif

#endif /* _DOLPHIN_MTX */
