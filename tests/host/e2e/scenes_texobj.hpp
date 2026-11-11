// GXTexObj register round-trip scenes for e2e_main.cpp. Included inside its anonymous namespace, after the helpers and
// before make_scenes().
//
// Why this file exists: HANDOVER §7.4b records two fixes whose only evidence was arithmetic done by hand, and a test that
// did not exist. `overlay/gfx/texture.hpp` had two entries of kHwToGxFilter swapped, and negative LOD biases were cast
// float -> u8 (saturating to 0 on ARM). Neither had any coverage: `grep -rn 'min_filter\|lod_bias' tests/host/` was empty,
// so both could have been "fixed" the same day upstream fixed them while the tests stayed green. These scenes drive the
// real GX API (GXInitTexObjLOD / GXInitTexObjFilter / GXInitTexObjLODBias / GXLoadTexObj), go through the real FIFO into
// the real command_processor, and read back what `gxState().loadedTextures` decoded. The expected values are derived
// from the GX hardware definitions below, NOT from the decoder under test.
//
// The register encoding, as GXTexture.cpp writes it:
//   mode0 bits  5..7   min filter, GX2HWFiltConv[minFilt]
//   mode0 bit   4      mag filter, 1 == GX_LINEAR
//   mode0 bits  9..16  LOD bias, int8, 1/32 per step
//   mode1 bits  0..7   min LOD, 1/16 per step
//   mode1 bits  8..15  max LOD, 1/16 per step
// with GX2HWFiltConv = {0x00, 0x04, 0x01, 0x05, 0x02, 0x06} over
// {GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN},
// and GXInitTexObjLOD clamping the bias to [-4.0, 3.99] and the LODs to [0, 10].
#pragma once

// ---- compile-time proof that the LOD-bias encoding is not undefined behaviour ------------------------
//
// The LOD-bias defect (patches.py -> dolphin/gx/GXTexture.cpp) was `(uint8_t)(32.0f * clampedBias)`: a
// negative float converted straight to an unsigned type. That is UB, and it is NOT detectable by any
// runtime test on x86, which is worth writing down because it cost a wasted test here:
//   * at runtime gcc emits cvttss2si then stores the low byte, so -128.0f becomes 0x80, the correct
//     answer, in BOTH the buggy and the fixed form. A round-trip scene passes either way.
//   * at compile time gcc FOLDS it to 0 instead. Same expression, two different answers -- the
//     signature of UB. `(uint8_t)(32.0f * -4.0f)` is 0 folded and 128 at runtime on x86-64.
//   * on ARM the hardware saturates, which is the form the hardware reported.
// So the fix can only be pinned at compile time: in a constant expression the conversion is
// ill-formed ("overflow in constant expression"), and the fixed form is well defined because
// integral conversions are modulo. The two assertions below fail the build if anyone reverts it.
// They mirror the exact expression in GXTexture.cpp, arguments included (the clamp to [-4, 3.99]).
constexpr uint8_t encode_lod_bias(float clampedBias) {
  return static_cast<uint8_t>(static_cast<int>(32.0f * clampedBias));
}
// -4.0 (the clamp floor) must encode as -128, i.e. 0x80 in the register's 8-bit field.
static_assert(encode_lod_bias(-4.0f) == 0x80u, "LOD bias -4.0 must encode as -128 (0x80)");
// -1.0 must not collapse to 0, which is what the saturating ARM conversion did.
static_assert(encode_lod_bias(-1.0f) == 0xE0u, "LOD bias -1.0 must encode as -32 (0xE0), not 0");
static_assert(encode_lod_bias(0.0f) == 0u, "LOD bias 0 must encode as 0");
// 3.99 is the clamp ceiling; 32 * 3.99 = 127.68 truncates to 127.
static_assert(encode_lod_bias(3.99f) == 127u, "LOD bias 3.99 must encode as 127");

// A 4x4 RGBA8 texture is enough: nothing here samples it, the object only has to be loadable.
void texobj_roundtrip_setup(GXTexObj& obj, std::array<uint32_t, 64>& image) {
  image.fill(0xFFFFFFFFu);
  GXInitTexObj(&obj, image.data(), 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
}

// Every GXTexFilter value, both as the min filter (the swapped table) and as the mag filter.
void add_texobj_scenes(std::vector<Scene>& s) {
  s.push_back(
      {"texminfilter_roundtrip",
       "every GXTexFilter survives GXInitTexObjLOD -> FIFO -> decode as the same enum (pins kHwToGxFilter)",
       [] {
         base_state();
         // GXLoadTexObj for a fresh map each iteration: the decode is what is under test, not any cache behaviour.
         static const GXTexFilter kFilters[6] = {GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR,
                                                 GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN};
         static const GXTexMapID kIds[6] = {GX_TEXMAP0, GX_TEXMAP1, GX_TEXMAP2,
                                            GX_TEXMAP3, GX_TEXMAP4, GX_TEXMAP5};
         static std::array<std::array<uint32_t, 64>, 6> sImages{};
         static GXTexObj sObjs[6]{};
         for (int i = 0; i < 6; ++i) {
           texobj_roundtrip_setup(sObjs[i], sImages[i]);
           // mag filter GX_NEAR, so a min/mag mix-up in the encoding shows up as a wrong mag value too.
           GXInitTexObjLOD(&sObjs[i], kFilters[i], GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
           GXLoadTexObj(&sObjs[i], kIds[i]);
         }
       },
       [](const Image&, Checker& c) {
         static const GXTexFilter kFilters[6] = {GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR,
                                                 GX_LIN_MIP_NEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN};
         static const char* kNames[6] = {"NEAR", "LINEAR", "NEAR_MIP_NEAR", "LIN_MIP_NEAR", "NEAR_MIP_LIN",
                                         "LIN_MIP_LIN"};
         for (int i = 0; i < 6; ++i) {
           const auto& slot = aurora::gx::g_gxState.loadedTextures[static_cast<int>(GX_TEXMAP0) + i];
           char buf[160];
           std::snprintf(buf, sizeof(buf), "GX_TEXMAP%d min_filter = %d, expected %s (%d)", i,
                         static_cast<int>(slot.min_filter()), kNames[i], static_cast<int>(kFilters[i]));
           c.is(slot.min_filter() == kFilters[i], buf);
         }
       },
       nullptr});

  s.push_back({"texmagfilter_roundtrip",
               "GX_LINEAR / GX_NEAR survive as the mag filter, and do not disturb the min filter",
               [] {
                 base_state();
                 static std::array<uint32_t, 64> sImageA, sImageB;
                 static GXTexObj sObjA, sObjB;
                 texobj_roundtrip_setup(sObjA, sImageA);
                 texobj_roundtrip_setup(sObjB, sImageB);
                 // LIN_MIP_NEAR is the interesting min filter here: it is one of the two the old table got wrong.
                 GXInitTexObjLOD(&sObjA, GX_LIN_MIP_NEAR, GX_LINEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
                 GXLoadTexObj(&sObjA, GX_TEXMAP0);
                 GXInitTexObjLOD(&sObjB, GX_NEAR_MIP_LIN, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
                 GXLoadTexObj(&sObjB, GX_TEXMAP1);
               },
               [](const Image&, Checker& c) {
                 const auto& a = aurora::gx::g_gxState.loadedTextures[static_cast<int>(GX_TEXMAP0)];
                 const auto& b = aurora::gx::g_gxState.loadedTextures[static_cast<int>(GX_TEXMAP1)];
                 c.is(a.mag_filter() == GX_LINEAR, "GX_TEXMAP0 mag_filter = GX_LINEAR");
                 c.is(a.min_filter() == GX_LIN_MIP_NEAR, "GX_TEXMAP0 min_filter = GX_LIN_MIP_NEAR (mag filter is a "
                                                        "separate bit, not a shift of the min field)");
                 c.is(b.mag_filter() == GX_NEAR, "GX_TEXMAP1 mag_filter = GX_NEAR");
                 c.is(b.min_filter() == GX_NEAR_MIP_LIN, "GX_TEXMAP1 min_filter = GX_NEAR_MIP_LIN");
               },
               nullptr});

  s.push_back({"texlodbias_roundtrip",
               "bias survives as signed 1/32 steps; the float->u8 UB itself is pinned by the static_asserts above, "
               "because x86 cannot detect it at runtime (see the note at the top of this file)",
               [] {
                 base_state();
                 // Biases chosen so each one exercises a different code path of the encoding: an exact multiple of
                 // 1/32, a value that needs truncation, both clamp ends, and -1 (the value that used to become 0).
                 static const float kBias[6] = {-4.0f, -1.5f, -0.5f, 0.0f, 1.5f, 3.99f};
                 static const GXTexMapID kIds[6] = {GX_TEXMAP0, GX_TEXMAP1, GX_TEXMAP2,
                                                    GX_TEXMAP3, GX_TEXMAP4, GX_TEXMAP5};
                 static std::array<std::array<uint32_t, 64>, 6> sImages{};
                 static GXTexObj sObjs[6]{};
                 for (int i = 0; i < 6; ++i) {
                   texobj_roundtrip_setup(sObjs[i], sImages[i]);
                   GXInitTexObjLOD(&sObjs[i], GX_LIN_MIP_LIN, GX_LINEAR, 0.f, 6.f, kBias[i], GX_FALSE, GX_FALSE,
                                   GX_ANISO_1);
                   GXLoadTexObj(&sObjs[i], kIds[i]);
                 }
               },
               [](const Image&, Checker& c) {
                 static const float kBias[6] = {-4.0f, -1.5f, -0.5f, 0.0f, 1.5f, 3.99f};
                 for (int i = 0; i < 6; ++i) {
                   const auto& slot = aurora::gx::g_gxState.loadedTextures[static_cast<int>(GX_TEXMAP0) + i];
                   // 3.99f clamps to 3.99 then truncates to int(127.68) = 127, i.e. 127/32 = 3.96875.
                   const float expected = i == 5 ? 127.0f / 32.0f : kBias[i];
                   char buf[160];
                   std::snprintf(buf, sizeof(buf), "GX_TEXMAP%d lod_bias = %.5f, expected %.5f", i,
                                 static_cast<double>(slot.lod_bias()), static_cast<double>(expected));
                   // Exactly representable in both cases, so equality is the honest check here.
                   c.is(slot.lod_bias() == expected, buf);
                 }
               },
               nullptr});

  s.push_back({"texlod_limits_roundtrip",
               "min/max LOD survive as 1/16 steps and are clamped to [0, 10]",
               [] {
                 base_state();
                 static const float kMin[4] = {0.0f, 1.5f, 5.0f, 10.0f};
                 static const float kMax[4] = {0.0f, 2.5f, 6.0f, 10.0f};
                 static const GXTexMapID kIds[4] = {GX_TEXMAP0, GX_TEXMAP1, GX_TEXMAP2, GX_TEXMAP3};
                 static std::array<std::array<uint32_t, 64>, 4> sImages{};
                 static GXTexObj sObjs[4]{};
                 for (int i = 0; i < 4; ++i) {
                   texobj_roundtrip_setup(sObjs[i], sImages[i]);
                   GXInitTexObjLOD(&sObjs[i], GX_LIN_MIP_LIN, GX_LINEAR, kMin[i], kMax[i], 0.f, GX_FALSE, GX_FALSE,
                                   GX_ANISO_1);
                   GXLoadTexObj(&sObjs[i], kIds[i]);
                 }
               },
               [](const Image&, Checker& c) {
                 static const float kMin[4] = {0.0f, 1.5f, 5.0f, 10.0f};
                 static const float kMax[4] = {0.0f, 2.5f, 6.0f, 10.0f};
                 for (int i = 0; i < 4; ++i) {
                   const auto& slot = aurora::gx::g_gxState.loadedTextures[static_cast<int>(GX_TEXMAP0) + i];
                   char buf[160];
                   std::snprintf(buf, sizeof(buf), "GX_TEXMAP%d min_lod = %.5f, expected %.5f", i,
                                 static_cast<double>(slot.min_lod()), static_cast<double>(kMin[i]));
                   c.is(slot.min_lod() == kMin[i], buf);
                   std::snprintf(buf, sizeof(buf), "GX_TEXMAP%d max_lod = %.5f, expected %.5f", i,
                                 static_cast<double>(slot.max_lod()), static_cast<double>(kMax[i]));
                   c.is(slot.max_lod() == kMax[i], buf);
                 }
               },
               nullptr});
}
