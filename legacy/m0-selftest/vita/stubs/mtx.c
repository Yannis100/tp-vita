/*
 * mtx.c - portable C implementations of the Dolphin matrix/vector/quaternion
 * math used by TP's JSystem/JMath game logic.
 *
 * Upstream (libs/revolution/src/mtx/*.c) is Metrowerks-only PPC assembly, so a
 * Vita/ARM build needs portable C equivalents. These are correctness-focused
 * (not cycle-matched); the decomp's own JMath/JMA* fallbacks are unchanged.
 */
#include <dolphin/mtx.h>

#include <math.h>
#include <string.h>

/* ---- Matrix 3x4 ---- */

void C_MTXIdentity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = 1.0f;
    m[1][1] = 1.0f;
    m[2][2] = 1.0f;
}

void C_MTXCopy(const Mtx src, Mtx dst) {
    memcpy(dst, src, sizeof(Mtx));
}

void C_MTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx tmp;
    /* Affine 3x4 concat; implicit fourth rows are [0 0 0 1]. */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
        tmp[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }
    memcpy(ab, tmp, sizeof(Mtx));
}

void C_MTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        C_MTXConcat(a, srcBase[i], dstBase[i]);
    }
}

void C_MTXTranspose(const Mtx src, Mtx xPose) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            xPose[j][i] = src[i][j];
        }
    }
    // last row = [0 0 0 1]
    xPose[3][0] = xPose[3][1] = xPose[3][2] = 0.0f;
    xPose[3][3] = 1.0f;
}

u32 C_MTXInverse(const Mtx src, Mtx inv) {
    /* Adjugate of the affine 3x3 part. */
    f32 m00 = src[0][0], m01 = src[0][1], m02 = src[0][2], m03 = src[0][3];
    f32 m10 = src[1][0], m11 = src[1][1], m12 = src[1][2], m13 = src[1][3];
    f32 m20 = src[2][0], m21 = src[2][1], m22 = src[2][2], m23 = src[2][3];

    f32 det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) +
              m02 * (m10 * m21 - m11 * m20);
    if (det == 0.0f) {
        return 0;
    }
    f32 invDet = 1.0f / det;

    inv[0][0] = (m11 * m22 - m12 * m21) * invDet;
    inv[0][1] = (m02 * m21 - m01 * m22) * invDet;
    inv[0][2] = (m01 * m12 - m02 * m11) * invDet;
    inv[0][3] = -(m03 * inv[0][0] + m13 * inv[0][1] + m23 * inv[0][2]);

    inv[1][0] = (m12 * m20 - m10 * m22) * invDet;
    inv[1][1] = (m00 * m22 - m02 * m20) * invDet;
    inv[1][2] = (m02 * m10 - m00 * m12) * invDet;
    inv[1][3] = -(m03 * inv[1][0] + m13 * inv[1][1] + m23 * inv[1][2]);

    inv[2][0] = (m10 * m21 - m11 * m20) * invDet;
    inv[2][1] = (m01 * m20 - m00 * m21) * invDet;
    inv[2][2] = (m00 * m11 - m01 * m10) * invDet;
    inv[2][3] = -(m03 * inv[2][0] + m13 * inv[2][1] + m23 * inv[2][2]);

    return 1;
}

u32 C_MTXInvXpose(const Mtx src, Mtx invX) {
    Mtx inv;
    if (!C_MTXInverse(src, inv)) {
        return 0;
    }
    C_MTXTranspose(inv, invX);
    return 1;
}

/* ---- Matrix * vector ---- */

void C_MTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    dst->x = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    dst->y = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    dst->z = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
}

void C_MTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        C_MTXMultVec(m, &srcBase[i], &dstBase[i]);
    }
}

void C_MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    C_MTXMultVec(m, src, dst);
}

void C_MTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}

void C_MTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
        dstBase[i].y = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
        dstBase[i].z = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    }
}

void C_MTXMultS16VecArray(const Mtx m, const S16Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        dstBase[i].y = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        dstBase[i].z = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    }
}

void C_MTXROMultS16VecArray(const ROMtx m, const S16Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        f32 x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z;
        dstBase[i].x = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
        dstBase[i].y = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
        dstBase[i].z = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    }
}

/* ---- Matrix transforms ---- */

void C_MTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    C_MTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void C_MTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    C_MTXCopy(src, dst);
    dst[0][3] += xT;
    dst[1][3] += yT;
    dst[2][3] += zT;
}

void C_MTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    C_MTXIdentity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void C_MTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            f32 s = (i == 0) ? xS : (i == 1) ? yS : zS;
            dst[i][j] = src[i][j] * s;
        }
        dst[i][3] = src[i][3];
    }
}

void C_MTXRotRad(Mtx m, char axis, f32 rad) {
    C_MTXIdentity(m);
    f32 s = sinf(rad), c = cosf(rad);
    switch (axis) {
    case 'x':
        m[1][1] = c; m[1][2] = -s;
        m[2][1] = s; m[2][2] = c;
        break;
    case 'y':
        m[0][0] = c; m[0][2] = s;
        m[2][0] = -s; m[2][2] = c;
        break;
    case 'z':
        m[0][0] = c; m[0][1] = -s;
        m[1][0] = s; m[1][1] = c;
        break;
    }
}

void C_MTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) {
    C_MTXIdentity(m);
    switch (axis) {
    case 'x':
        m[1][1] = cosA; m[1][2] = -sinA;
        m[2][1] = sinA; m[2][2] = cosA;
        break;
    case 'y':
        m[0][0] = cosA; m[0][2] = sinA;
        m[2][0] = -sinA; m[2][2] = cosA;
        break;
    case 'z':
        m[0][0] = cosA; m[0][1] = -sinA;
        m[1][0] = sinA; m[1][1] = cosA;
        break;
    }
}

void C_MTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    f32 x = axis->x, y = axis->y, z = axis->z;
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    m[0][0] = t * x * x + c;
    m[0][1] = t * x * y - s * z;
    m[0][2] = t * x * z + s * y;
    m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z;
    m[1][1] = t * y * y + c;
    m[1][2] = t * y * z - s * x;
    m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y;
    m[2][1] = t * y * z + s * x;
    m[2][2] = t * z * z + c;
    m[2][3] = 0.0f;
}

void C_MTXQuat(Mtx m, const Quaternion* q) {
    f32 x = q->x, y = q->y, z = q->z, w = q->w;
    f32 xx = x * x, yy = y * y, zz = z * z;
    f32 xy = x * y, xz = x * z, yz = y * z;
    f32 wx = w * x, wy = w * y, wz = w * z;
    m[0][0] = 1 - 2 * (yy + zz);
    m[0][1] = 2 * (xy - wz);
    m[0][2] = 2 * (xz + wy);
    m[0][3] = 0.0f;
    m[1][0] = 2 * (xy + wz);
    m[1][1] = 1 - 2 * (xx + zz);
    m[1][2] = 2 * (yz - wx);
    m[1][3] = 0.0f;
    m[2][0] = 2 * (xz - wy);
    m[2][1] = 2 * (yz + wx);
    m[2][2] = 1 - 2 * (xx + yy);
    m[2][3] = 0.0f;
}

void C_MTXReflect(Mtx m, const Vec* p, const Vec* n) {
    C_MTXIdentity(m);
    f32 a = n->x, b = n->y, c = n->z;
    f32 two = 2.0f;
    m[0][0] = 1 - two * a * a;
    m[0][1] = -two * a * b;
    m[0][2] = -two * a * c;
    m[0][3] = two * a * (p->x * a + p->y * b + p->z * c);
    m[1][0] = -two * b * a;
    m[1][1] = 1 - two * b * b;
    m[1][2] = -two * b * c;
    m[1][3] = two * b * (p->x * a + p->y * b + p->z * c);
    m[2][0] = -two * c * a;
    m[2][1] = -two * c * b;
    m[2][2] = 1 - two * c * c;
    m[2][3] = two * c * (p->x * a + p->y * b + p->z * c);
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target) {
    Vec fwd = {target->x - camPos->x, target->y - camPos->y, target->z - camPos->z};
    C_VECNormalize(&fwd, &fwd);
    Vec side, up;
    C_VECCross(&fwd, camUp, &side);
    C_VECNormalize(&side, &side);
    C_VECCross(&side, &fwd, &up);

    m[0][0] = side.x; m[0][1] = side.y; m[0][2] = side.z;
    m[0][3] = -(side.x * camPos->x + side.y * camPos->y + side.z * camPos->z);
    m[1][0] = up.x; m[1][1] = up.y; m[1][2] = up.z;
    m[1][3] = -(up.x * camPos->x + up.y * camPos->y + up.z * camPos->z);
    m[2][0] = -fwd.x; m[2][1] = -fwd.y; m[2][2] = -fwd.z;
    m[2][3] = fwd.x * camPos->x + fwd.y * camPos->y + fwd.z * camPos->z;
}

void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = 2 * n / (r - l);
    m[0][2] = (r + l) / (r - l);
    m[1][1] = 2 * n / (t - b);
    m[1][2] = (t + b) / (t - b);
    m[2][2] = -(f + n) / (f - n);
    m[2][3] = -2 * f * n / (f - n);
    m[3][2] = -1.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 t = n * tanf(fovY * 0.5f);
    f32 b = -t;
    f32 r = t * aspect;
    f32 l = -r;
    C_MTXFrustum(m, t, b, l, r, n, f);
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = 2 / (r - l);
    m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2 / (t - b);
    m[1][3] = -(t + b) / (t - b);
    m[2][2] = -2 / (f - n);
    m[2][3] = -(f + n) / (f - n);
    m[3][3] = 1.0f;
}

void C_MTXLighting(Mtx44 m, const Vec* pos, const Vec* up, const Vec* left) {
    Vec right;
    C_VECCross(left, up, &right);
    C_MTXIdentity(m);
    m[0][0] = left->x; m[0][1] = left->y; m[0][2] = left->z; m[0][3] = -C_VECDotProduct(left, pos);
    m[1][0] = up->x; m[1][1] = up->y; m[1][2] = up->z; m[1][3] = -C_VECDotProduct(up, pos);
    m[2][0] = right.x; m[2][1] = right.y; m[2][2] = right.z;
    m[2][3] = -C_VECDotProduct(&right, pos);
}

/* ---- Vector ---- */

void C_VECAdd(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x + b->x;
    ab->y = a->y + b->y;
    ab->z = a->z + b->z;
}

void C_VECSubtract(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x - b->x;
    ab->y = a->y - b->y;
    ab->z = a->z - b->z;
}

f32 C_VECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void C_VECCross(const Vec* a, const Vec* b, Vec* axb) {
    axb->x = a->y * b->z - a->z * b->y;
    axb->y = a->z * b->x - a->x * b->z;
    axb->z = a->x * b->y - a->y * b->x;
}

f32 C_VECSquareMag(const Vec* a) {
    return C_VECDotProduct(a, a);
}

f32 C_VECMag(const Vec* a) {
    return sqrtf(C_VECSquareMag(a));
}

void C_VECNormalize(const Vec* src, Vec* dst) {
    f32 mag = C_VECMag(src);
    if (mag == 0.0f) {
        *dst = *src;
        return;
    }
    f32 inv = 1.0f / mag;
    dst->x = src->x * inv;
    dst->y = src->y * inv;
    dst->z = src->z * inv;
}

f32 C_VECDistance(const Vec* a, const Vec* b) {
    Vec d;
    C_VECSubtract(a, b, &d);
    return C_VECMag(&d);
}

void C_VECScale(const Vec* src, Vec* dst, f32 scale) {
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

void C_VECAddScaled(const Vec* a, const Vec* b, Vec* ab, f32 scale) {
    ab->x = a->x + b->x * scale;
    ab->y = a->y + b->y * scale;
    ab->z = a->z + b->z * scale;
}

/* ---- Quaternion ---- */

void C_QUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    /* Compute into locals first: callers may pass r aliasing p or q. */
    f32 rx = p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y;
    f32 ry = p->w * q->y - p->x * q->z + p->y * q->w + p->z * q->x;
    f32 rz = p->w * q->z + p->x * q->y - p->y * q->x + p->z * q->w;
    f32 rw = p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z;
    r->x = rx;
    r->y = ry;
    r->z = rz;
    r->w = rw;
}

void C_QUATDivide(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    Quaternion qInv = {-q->x, -q->y, -q->z, q->w};
    f32 mag = C_VECSquareMag((const Vec*)q) + q->w * q->w;
    if (mag != 0.0f) {
        mag = 1.0f / mag;
    }
    qInv.x *= mag;
    qInv.y *= mag;
    qInv.z *= mag;
    qInv.w *= mag;
    C_QUATMultiply(p, &qInv, r);
}

void C_QUATScale(const Quaternion* p, Quaternion* r, f32 scale) {
    r->x = p->x * scale;
    r->y = p->y * scale;
    r->z = p->z * scale;
    r->w = p->w * scale;
}

void C_QUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    r->x = p->x + q->x;
    r->y = p->y + q->y;
    r->z = p->z + q->z;
    r->w = p->w + q->w;
}

void C_QUATSubtract(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    r->x = p->x - q->x;
    r->y = p->y - q->y;
    r->z = p->z - q->z;
    r->w = p->w - q->w;
}

void C_QUATDot(const Quaternion* p, const Quaternion* q, f32* dot) {
    *dot = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
}

void C_QUATNormalize(const Quaternion* p, Quaternion* r) {
    f32 mag = sqrtf(C_VECSquareMag((const Vec*)p) + p->w * p->w);
    if (mag == 0.0f) {
        *r = *p;
        return;
    }
    f32 inv = 1.0f / mag;
    C_QUATScale(p, r, inv);
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 dot = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    if (dot < 0.0f) {
        Quaternion neg = {-q->x, -q->y, -q->z, -q->w};
        C_QUATSlerp(p, &neg, r, t);
        return;
    }
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    f32 theta = acosf(dot);
    if (fabsf(theta) < 1e-6f) {
        C_QUATScale(p, r, 1.0f - t);
        Quaternion qs;
        C_QUATScale(q, &qs, t);
        C_QUATAdd(r, &qs, r);
        return;
    }
    f32 sinTheta = sinf(theta);
    f32 w0 = sinf((1.0f - t) * theta) / sinTheta;
    f32 w1 = sinf(t * theta) / sinTheta;
    C_QUATScale(p, r, w0);
    Quaternion qs;
    C_QUATScale(q, &qs, w1);
    C_QUATAdd(r, &qs, r);
}

void C_QUATExp(const Quaternion* p, Quaternion* r) {
    f32 vn = sqrtf(p->x * p->x + p->y * p->y + p->z * p->z);
    f32 s = vn > 1e-6f ? sinf(vn) / vn : 1.0f;
    r->x = p->x * s;
    r->y = p->y * s;
    r->z = p->z * s;
    r->w = cosf(vn);
}

void C_QUATLog(const Quaternion* p, Quaternion* r) {
    f32 vn = sqrtf(p->x * p->x + p->y * p->y + p->z * p->z);
    f32 scale = vn > 1e-6f ? atan2f(vn, p->w) / vn : 0.0f;
    r->x = p->x * scale;
    r->y = p->y * scale;
    r->z = p->z * scale;
    r->w = 0.0f;
}

void C_QUATAxisRad(Quaternion* r, const Vec* axis, f32 rad) {
    f32 half = rad * 0.5f;
    f32 s = sinf(half);
    r->x = axis->x * s;
    r->y = axis->y * s;
    r->z = axis->z * s;
    r->w = cosf(half);
}

void C_QUATMatrix(const Quaternion* q, Mtx m) {
    C_MTXQuat(m, q);
}

void C_QUATRotVec(const Quaternion* q, const Vec* src, Vec* dst) {
    Quaternion p = {src->x, src->y, src->z, 0.0f};
    Quaternion qInv = {-q->x, -q->y, -q->z, q->w};
    Quaternion tmp;
    C_QUATMultiply(q, &p, &tmp);
    C_QUATMultiply(&tmp, &qInv, &tmp);
    dst->x = tmp.x;
    dst->y = tmp.y;
    dst->z = tmp.z;
}

void C_MTXReorder(const Mtx src, ROMtx dest) {
    for (int i = 0; i < 3; i++) {
        dest[0][i] = src[i][0];
        dest[1][i] = src[i][1];
        dest[2][i] = src[i][2];
        dest[3][i] = src[i][3];
    }
}
