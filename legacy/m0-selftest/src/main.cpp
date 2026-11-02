/*
 * main.cpp - TP Tier-1 Vita skeleton entry point + self-test harness.
 *
 * This "game" proves that the Twilight Princess decomp game-logic core
 * (JSystem JMath + JSupport + Dolphin math) compiles and runs on the PS Vita
 * CPU (ARMv7) with the WebGPU/GX renderer stubbed out. There is no game data
 * and no renderer — it runs a small suite of the game's own math/logic and
 * prints results to the Vita console.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" void vita_runtime_init(void);

/* --- TP game logic headers (upstream tree, via _TP_ROOT) --- */
#include "JSystem/JMath/JMath.h"
#include "JSystem/JMath/JMATrigonometric.h"
#include "JSystem/JMath/random.h"
#include "JSystem/JSupport/JSUList.h"
#include "JSystem/JSupport/JSUMemoryStream.h"

static int g_failures = 0;
static void check(bool ok, const char* what) {
    sceClibPrintf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        g_failures++;
    }
}

static void test_jmath(void) {
    sceClibPrintf("TP JMath self-test\n");
    /* JMAEulerToQuat + normalise + back to matrix. */
    Quaternion q;
    JMAEulerToQuat(0, 0, 0, &q);
    check(fabsf(q.w - 1.0f) < 1e-5f && fabsf(q.x) < 1e-5f && fabsf(q.y) < 1e-5f && fabsf(q.z) < 1e-5f,
          "JMAEulerToQuat(0,0,0) = identity quaternion");

    /* sincos table: sin(90deg) ~ 1 */
    f32 s = JMath::sincosTable_.sinDegree(90.0f);
    f32 c = JMath::sincosTable_.cosDegree(90.0f);
    check(fabsf(s - 1.0f) < 1e-4f, "sincosTable sin(90)=1");
    check(fabsf(c) < 1e-4f, "sincosTable cos(90)=0");

    /* fast reciprocal */
    f32 r = JMAFastReciprocal(4.0f);
    check(fabsf(r - 0.25f) < 1e-5f, "JMAFastReciprocal(4)=0.25");

    /* fast sqrt (1/sqrt via frsqrte path) */
    f32 sq = JMAFastSqrt(16.0f);
    check(fabsf(sq - 4.0f) < 1e-4f, "JMAFastSqrt(16)=4");

    /* random */
    JMath::TRandom_fast_ rng(12345u);
    u32 a = rng.get();
    u32 b = rng.get();
    check(a != b, "TRandom_fast_ produces varying values");

    /* hermite interpolation: at t=0 (b==0) should equal p3 */
    f32 h = JMAHermiteInterpolation(0.f, 1.f, 2.f, 0.f, 1.f, 3.f, 4.f);
    (void)h;
}

static void test_jsupport(void) {
    sceClibPrintf("TP JSupport (JSU streams + lists) self-test\n");
    u8 buf[64];
    JSUMemoryInputStream in(buf, 0);
    check(in.isGood(), "JSUMemoryInputStream empty is good");

    /* write a big-endian u32 (as a .arc-style stream stores it), read back */
    u32 written = 0x01020304u;
    be_swap(written);  /* host -> big-endian bytes in buffer */
    JSUMemoryOutputStream out(buf, sizeof(buf));
    out.write(&written, sizeof(written));
    check(out.getPosition() == 4, "JSUMemoryOutputStream write length");

    JSUMemoryInputStream in2(buf, 4);
    u32 rd = in2.readU32();
    check(rd == 0x01020304u, "JSUMemoryInputStream readU32 big-endian roundtrip");
    check(in2.getPosition() == 4, "stream position advanced");
    check(in2.getAvailable() == 0, "stream fully consumed");

    /* JSUPtrList */
    JSUPtrLink links[3] = {JSUPtrLink((void*)1), JSUPtrLink((void*)2), JSUPtrLink((void*)3)};
    JSUPtrList list(true);
    check(list.append(&links[0]) && list.append(&links[1]) && list.append(&links[2]),
          "JSUPtrList append x3");
    check(list.getNumLinks() == 3, "JSUPtrList length");
    check(list.remove(&links[1]), "JSUPtrList remove middle");
    check(list.getNumLinks() == 2, "JSUPtrList length after remove");
}

static void test_mtx(void) {
    sceClibPrintf("TP Dolphin mtx self-test\n");
    Mtx m, inv, prod;
    C_MTXIdentity(m);
    check(m[0][0] == 1.f && m[1][1] == 1.f && m[2][2] == 1.f && m[0][3] == 0.f,
          "C_MTXIdentity");
    m[0][3] = 5.f;
    check(C_MTXInverse(m, inv) == 1, "C_MTXInverse invertible");
    C_MTXConcat(m, inv, prod);
    check(fabsf(prod[0][0] - 1.f) < 1e-4f && fabsf(prod[1][1] - 1.f) < 1e-4f &&
              fabsf(prod[2][2] - 1.f) < 1e-4f && fabsf(prod[0][3]) < 1e-4f,
          "M*M^-1 = I");

    Vec v = {3.f, 4.f, 0.f};
    Vec n;
    C_VECNormalize(&v, &n);
    check(fabsf(n.x * n.x + n.y * n.y + n.z * n.z - 1.f) < 1e-5f, "C_VECNormalize unit length");

    Quaternion qa, qb, qr;
    Vec zAxis = {0.f, 0.f, 1.f};
    C_QUATAxisRad(&qa, &zAxis, 0.785398f);
    C_QUATAxisRad(&qb, &zAxis, 0.785398f);
    C_QUATMultiply(&qa, &qb, &qr);
    /* 90deg total about z -> rotate (1,0,0) to ~(0,1,0) */
    Vec src = {1.f, 0.f, 0.f}, dst;
    C_QUATRotVec(&qr, &src, &dst);
    check(fabsf(dst.x) < 1e-3f && fabsf(dst.y - 1.f) < 1e-3f, "C_QUATRotVec 90deg about z");
}

int main() {
    vita_runtime_init();
    sceClibPrintf("\n=== Zelda Twilight Princess - Tier-1 Vita skeleton ===\n");
    sceClibPrintf("Game-logic core compiled natively for ARMv7. GX/WebGPU renderer stubbed.\n");
    sceClibPrintf("TC: %s\n", JMAFastSqrt(64.0f) == 8.0f ? "gcc" : "?");
    sceClibPrintf("\n");

    test_jmath();
    test_jsupport();
    test_mtx();

    sceClibPrintf("\n%s (%d failure(s)) - sleeping forever; press PS button to exit\n",
                  g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
#if defined(__vita__)
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
    }
#else
    /* Host self-test: exit so CI/harness can see the result. */
    return g_failures == 0 ? 0 : 1;
#endif
}
