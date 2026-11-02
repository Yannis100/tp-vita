// EFB alpha as Twilight Princess uses it (included by e2e_main.cpp; the helpers live there).
//
// The harness presents RGB only, so the EFB alpha is read the way the game reads it: an EFB copy into an RGBA8 texture, drawn
// back with the TEV colour set to the texture's alpha (GX_CC_TEXA). The shown grey level IS the alpha.
//
// What TP does with alpha on this port (TARGET_PC is defined for the Vita build, so JFWDisplay::clearEfb(5 args), the only
// caller of GXSetDstAlpha and GXSetZTexture(GX_ZT_REPLACE), returns on its first line: its clear quad is NOT a live path):
//   JFWDisplay::preGX            GXSetPixelFmt(GX_PF_RGBA6_Z24) every frame (m_Do_graphic.cpp:307 passes enableAlpha = true),
//                                GXSetAlphaUpdate(TRUE)
//   J3DSys.cpp 371, d_a_player    GXSetAlphaUpdate(FALSE) around every J3D material
//   d_drawlist.cpp 1547..1626     shadow images: offscreen target, blend ONE/ONE, three shadows packed into R, G, B and a
//                                 fourth into alpha (colour update off, alpha update on), then a half-size copy with clear
//   d_a_mirror.cpp 135, d_kankyo_rain.cpp 5026, d_particle.cpp 259   colour (and alpha) update off: depth-only draws
//   JPABaseShape.cpp 1397         the particle blend table contains GX_BL_DSTALPHA / GX_BL_INVDSTALPHA
// The dst-alpha scenes test the renderer feature (GXSetDstAlpha + alpha update) that clearEfb would use if it were not
// stubbed; they are labelled "not live" in their description. GXSetZTexture is decoded by nothing in the renderer (and is only
// called from the stubbed function), so there is no scene for it.
//
// Expectations are the ideal GX values: real hardware keeps 6 bits of alpha in RGBA6_Z24, this backend keeps 8.
namespace alpha {
constexpr int kN = 128;               // the EFB corner the scenes draw in and copy
constexpr int kPX = 300, kPY = 200;   // where the copy of that corner is shown
alignas(32) uint8_t gCopy[4][kN * kN * 4]; // only the addresses are used: copies live on the GPU
GXTexObj gObj[4];                     // must outlive the FIFO drain

void rgba6() { GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR); }

// EFB rectangle -> texture slot `slot`.
void copy_to(int slot, int x, int y, int w, int h, GXTexFmt fmt = GX_TF_RGBA8, GXBool clear = GX_FALSE) {
  GXSetTexCopySrc(x, y, w, h);
  GXSetTexCopyDst(w, h, fmt, GX_FALSE);
  GXCopyTex(gCopy[slot], clear);
}

// Draws slot `slot` (w x h texels, format fmt) at (dx, dy) magnified by `zoom`, nearest filtered. `alphaAsGrey`: TEV colour =
// texture alpha, otherwise the texture's own colour. Resets the draw state first (base_state), so it is safe to call after
// any scene state.
void show(int slot, int w, int h, GXTexFmt fmt, int dx, int dy, int zoom, bool alphaAsGrey) {
  base_state();
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXInitTexObj(&gObj[slot], gCopy[slot], w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(&gObj[slot], GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(&gObj[slot], GX_TEXMAP0);
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  if (alphaAsGrey) {
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXA);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  } else {
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  }
  tex_rect(dx, dy, dx + w * zoom, dy + h * zoom, 1);
}
void show_alpha(int slot, int w, int h, int dx, int dy, GXTexFmt fmt = GX_TF_RGBA8, int zoom = 1) {
  show(slot, w, h, fmt, dx, dy, zoom, true);
}

// The grey level at probe position (px, py) inside a probe drawn at (dx, dy), must be the alpha `a` (+-2).
bool alpha_is(const Image& img, Checker& c, int dx, int dy, int px, int py, int a, const char* what) {
  const auto p = img.at(dx + px, dy + py);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s: EFB alpha %d, expected %d (probe pixel (%d,%d) = %d,%d,%d)", what, p[0], a, px, py,
                p[0], p[1], p[2]);
  return c.is(std::abs(p[0] - a) <= 2 && std::abs(p[1] - a) <= 2 && std::abs(p[2] - a) <= 2, buf);
}

constexpr int kQ = 64; // quadrant edge of the 2x2 scenes
struct Quad {
  int x, y;
};
constexpr Quad kQuads[4] = {{0, 0}, {kQ, 0}, {0, kQ}, {kQ, kQ}};
void fill(const Quad& q, u8 r, u8 g, u8 b, u8 a) { rect(q.x, q.y, q.x + kQ, q.y + kQ, 1, r, g, b, a); }
// centre of a quadrant, in the corner and in the probe
int cx(int quad) { return kQuads[quad].x + kQ / 2; }
int cy(int quad) { return kQuads[quad].y + kQ / 2; }
} // namespace alpha

void add_alpha_scenes(std::vector<Scene>& s) {
  using namespace alpha;

  // The 2x2 truth table of GXSetColorUpdate x GXSetAlphaUpdate. TP: shadow images pack a shadow into alpha only (colour update
  // off, alpha on); mirror / rain / particle depth passes switch both off; J3D draws with alpha off and colour on.
  s.push_back({"alpha_write_masks",
               "RGBA6 EFB: colour update x alpha update truth table; each plane changes only where its bit is set "
               "(d_drawlist.cpp 1602 alpha-only pass, d_a_mirror.cpp 135 depth-only draw)",
               [] {
                 base_state();
                 rgba6();
                 const struct {
                   bool color, alpha;
                   u8 r, g, b;
                 } over[4] = {{true, true, 200, 0, 0}, {false, true, 0, 255, 0}, {true, false, 0, 0, 255}, {false, false, 255, 255, 0}};
                 for (const Quad& q : kQuads) {
                   fill(q, 10, 20, 30, 200);
                 }
                 for (int i = 0; i < 4; ++i) {
                   GXSetColorUpdate(over[i].color);
                   GXSetAlphaUpdate(over[i].alpha);
                   fill(kQuads[i], over[i].r, over[i].g, over[i].b, 90);
                 }
                 GXSetColorUpdate(GX_TRUE);
                 GXSetAlphaUpdate(GX_TRUE);
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 // colour (presented EFB), then alpha (probe)
                 c.px(img, cx(0), cy(0), 200, 0, 0);
                 c.px(img, cx(1), cy(1), 10, 20, 30);
                 c.px(img, cx(2), cy(2), 0, 0, 255);
                 c.px(img, cx(3), cy(3), 10, 20, 30);
                 alpha_is(img, c, kPX, kPY, cx(0), cy(0), 90, "colour+alpha");
                 alpha_is(img, c, kPX, kPY, cx(1), cy(1), 90, "alpha only");
                 alpha_is(img, c, kPX, kPY, cx(2), cy(2), 200, "colour only");
                 alpha_is(img, c, kPX, kPY, cx(3), cy(3), 200, "neither");
               }});

  // d_drawlist.cpp dDlst_shadowControl_c::imageDraw (TARGET_PC branch), with the GX state of that function:
  //   GXCreateFrameBuffer(S, S) at chan 0, blend ONE/ONE, TEV colour C0 / alpha A0 from l_imageDrawColor[chan]
  //   ({255,0,0,0} {0,255,0,0} {0,0,255,0} {0,0,0,255}), chan 3 with colour update off and alpha update on, then
  //   GXSetTexCopySrc(0,0,S,S) + GXSetTexCopyDst(S/2,S/2,RGB5A3) + GXCopyTex(clear) with colour update back on,
  //   GXSetAlphaUpdate(FALSE) afterwards. A trailing partial batch recreates the target and copies it again.
  // The offscreen target starts at (0,0,0,0); the copy is half-size (a 2x2 box average); no batch leaks into the next.
  s.push_back({"tp_shadow_pack",
               "(d_drawlist.cpp 1547) offscreen 32x32, additive blend, R/G/B shadows + an alpha-only shadow, half-size RGB5A3 "
               "copy with clear; a second batch must not see the first; the EFB is untouched",
               [] {
                 constexpr int S = 32;
                 base_state();
                 rgba6();
                 rect(500, 50, 560, 110, 1, 255, 0, 255); // EFB content before the offscreen pass
                 f32 proj[4][4];
                 ortho(proj, 0.f, S, 0.f, S, 0.1f, 100.f);
                 const auto shadow_state = [&] {
                   GXSetProjection(proj, GX_ORTHOGRAPHIC);
                   GXClearVtxDesc();
                   GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                   GXSetNumChans(0);
                   GXSetNumTexGens(0);
                   GXSetNumTevStages(1);
                   GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
                   GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
                   GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
                   GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
                   GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
                   GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
                   GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
                 };
                 const auto shadow = [](int chan, f32 x0, f32 y0, f32 x1, f32 y1) {
                   static const GXColor kColor[4] = {{255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {0, 0, 0, 255}};
                   GXSetTevColor(GX_TEVREG0, kColor[chan]);
                   if (chan == 3) {
                     GXSetColorUpdate(GX_DISABLE);
                     GXSetAlphaUpdate(GX_ENABLE);
                   }
                   GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                   for (const auto& [x, y] : {std::pair{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}) {
                     GXPosition3f32(x, y, -1.f);
                   }
                   GXEnd();
                 };
                 const auto copy_half = [&](int slot) {
                   GXSetColorUpdate(GX_ENABLE);
                   GXSetTexCopySrc(0, 0, S, S);
                   GXSetTexCopyDst(S / 2, S / 2, GX_TF_RGB5A3, GX_TRUE);
                   GXCopyTex(gCopy[slot], GX_TRUE);
                   GXPixModeSync();
                   GXSetAlphaUpdate(GX_DISABLE);
                 };
                 // batch 1: four shadows
                 GXCreateFrameBuffer(S, S);
                 shadow_state();
                 GXSetAlphaUpdate(GX_DISABLE); // J3D leaves it off
                 shadow(0, 0, 0, 20, 20);
                 shadow(1, 11, 0, 31, 20);
                 shadow(2, 0, 10, 20, 30);
                 shadow(3, 11, 11, 31, 31);
                 copy_half(0);
                 // batch 2: the next loop iteration recreates the target and draws one shadow
                 GXCreateFrameBuffer(S, S);
                 shadow_state();
                 shadow(0, 4, 4, 13, 13);
                 copy_half(1);
                 GXRestoreFrameBuffer();
                 base_state();
                 rgba6();
                 rect(500, 150, 560, 210, 1, 0, 255, 255); // EFB content after it
                 show(0, 16, 16, GX_TF_RGB5A3, 300, 200, 4, false);
                 show_alpha(0, 16, 16, 400, 200, GX_TF_RGB5A3, 4);
                 show(1, 16, 16, GX_TF_RGB5A3, 300, 300, 4, false);
                 show_alpha(1, 16, 16, 400, 300, GX_TF_RGB5A3, 4);
               },
               [](const Image& img, Checker& c) {
                 const auto in = [](int x, int y, int x0, int y0, int x1, int y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; };
                 // ideal RGBA of source pixel (x, y), batch 1 and batch 2
                 const auto b1 = [&](int x, int y) {
                   return std::array<double, 4>{in(x, y, 0, 0, 20, 20) ? 255. : 0., in(x, y, 11, 0, 31, 20) ? 255. : 0.,
                                                in(x, y, 0, 10, 20, 30) ? 255. : 0., in(x, y, 11, 11, 31, 31) ? 255. : 0.};
                 };
                 const auto b2 = [&](int x, int y) {
                   return std::array<double, 4>{in(x, y, 4, 4, 13, 13) ? 255. : 0., 0., 0., 0.};
                 };
                 const auto texel = [&](const auto& f, int i, int j) { // half-size copy = 2x2 box average
                   std::array<double, 4> v{};
                   for (int dy = 0; dy < 2; ++dy) {
                     for (int dx = 0; dx < 2; ++dx) {
                       const auto p = f(2 * i + dx, 2 * j + dy);
                       for (int k = 0; k < 4; ++k) {
                         v[k] += p[k] / 4.;
                       }
                     }
                   }
                   return v;
                 };
                 for (int slot = 0; slot < 2; ++slot) {
                   const int rgbY = 200 + 100 * slot;
                   for (int j = 0; j < 16; ++j) {
                     for (int i = 0; i < 16; ++i) {
                       const auto v = slot == 0 ? texel(b1, i, j) : texel(b2, i, j);
                       char what[40];
                       std::snprintf(what, sizeof(what), "batch %d texel (%d,%d)", slot + 1, i, j);
                       c.px(img, 300 + 4 * i + 2, rgbY + 4 * j + 2, static_cast<int>(std::lround(v[0])),
                            static_cast<int>(std::lround(v[1])), static_cast<int>(std::lround(v[2])), 3);
                       alpha_is(img, c, 400, rgbY, 4 * i + 2, 4 * j + 2, static_cast<int>(std::lround(v[3])), what);
                     }
                   }
                 }
                 // the offscreen pass did not touch the EFB, before or after it
                 c.px(img, 530, 80, 255, 0, 255);
                 c.px(img, 530, 180, 0, 255, 255);
                 c.px(img, 10, 10, kGray, kGray, kGray);
                 c.px(img, 25, 25, kGray, kGray, kGray);
               }});

  // GXCopyTex(dest, clear = TRUE): the clear is subject to the colour and alpha update bits (GX docs; upstream
  // GXFrameBuffer.cpp: clearColor = clear && colorUpdate, clearAlpha = clear && alphaUpdate && RGBA6). The shadow pass above
  // depends on it twice: colour update is switched on before the copy, alpha update is still on.
  s.push_back({"efb_copy_clear_alpha",
               "GXCopyTex(clear) of a drawn rectangle: colour is cleared iff colour update, alpha iff alpha update (RGBA6); the "
               "clear colour is GXInit's (64,64,64,255)",
               [] {
                 base_state();
                 rgba6();
                 constexpr int E = 32; // each quadrant of the 64x64 region is copied (and cleared) on its own
                 const struct {
                   int x, y;
                   bool color, alpha;
                 } q[4] = {{0, 0, true, true}, {E, 0, true, false}, {0, E, false, true}, {E, E, false, false}};
                 for (const auto& p : q) {
                   rect(p.x, p.y, p.x + E, p.y + E, 1, 200, 0, 0, 90);
                 }
                 for (int i = 0; i < 4; ++i) {
                   GXSetColorUpdate(q[i].color);
                   GXSetAlphaUpdate(q[i].alpha);
                   copy_to(i, q[i].x, q[i].y, E, E, GX_TF_RGBA8, GX_TRUE);
                 }
                 GXSetColorUpdate(GX_TRUE);
                 GXSetAlphaUpdate(GX_TRUE);
                 copy_to(0, 0, 0, 2 * E, 2 * E);
                 show_alpha(0, 2 * E, 2 * E, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 const int m = 16; // quadrant centre offset
                 c.px(img, m, m, kGray, kGray, kGray);         // colour + alpha cleared
                 c.px(img, 32 + m, m, kGray, kGray, kGray);    // colour cleared only
                 c.px(img, m, 32 + m, 200, 0, 0);              // alpha cleared only
                 c.px(img, 32 + m, 32 + m, 200, 0, 0);         // nothing cleared
                 alpha_is(img, c, kPX, kPY, m, m, 255, "colour+alpha cleared (clear alpha 255)");
                 alpha_is(img, c, kPX, kPY, 32 + m, m, 90, "colour cleared only");
                 alpha_is(img, c, kPX, kPY, m, 32 + m, 255, "alpha cleared only");
                 alpha_is(img, c, kPX, kPY, 32 + m, 32 + m, 90, "nothing cleared");
               }});

  // The frame-start clear writes the clear colour's alpha (replay() clears colour, alpha and depth). The warmup frame fills
  // the EFB with alpha 10 and sets the clear colour (captured at the next begin_frame); the scene frame draws nothing.
  s.push_back({"efb_clear_alpha",
               "RGBA6: the frame-start clear writes the clear colour's alpha (77) over what the previous frame left (10)",
               [] {
                 base_state();
                 rgba6();
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
                 GXSetCopyClear(GXColor{64, 64, 64, 255}, 0xFFFFFF); // GXInit's, for the scenes after this one
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 20, 20, 30, 60, 90);
                 alpha_is(img, c, kPX, kPY, 20, 20, 77, "frame clear");
                 alpha_is(img, c, kPX, kPY, 100, 100, 77, "frame clear");
               },
               [] {
                 base_state();
                 rgba6();
                 rect(0, 0, W, H, 1, 255, 255, 255, 10);
                 GXSetCopyClear(GXColor{30, 60, 90, 77}, 0xFFFFFF);
               }});

  // Destination alpha (GXSetDstAlpha + alpha update): the constant replaces the TEV alpha. Not live in TP (see the header),
  // but implemented by the renderer: the fragment shader writes the constant itself (uExtra.x), except for a constant of 0,
  // which goes through the blend unit.
  s.push_back({"dst_alpha_replace",
               "(renderer feature, not live in TP) GXSetDstAlpha(ENABLE, 160) replaces the TEV alpha 40; disabled keeps 40; "
               "constant 0 clears alpha; alpha update off ignores it",
               [] {
                 base_state();
                 rgba6();
                 for (const Quad& q : kQuads) {
                   fill(q, 10, 20, 30, 200); // alpha 200 everywhere first
                 }
                 // Q0: constant 160 over TEV alpha 40
                 GXSetDstAlpha(GX_ENABLE, 160);
                 fill(kQuads[0], 200, 100, 0, 40);
                 // Q1: dst alpha disabled: the TEV alpha lands
                 GXSetDstAlpha(GX_DISABLE, 0);
                 fill(kQuads[1], 200, 100, 0, 40);
                 // Q2: constant 0
                 GXSetDstAlpha(GX_ENABLE, 0);
                 fill(kQuads[2], 200, 100, 0, 40);
                 // Q3: constant 90 but alpha update off: alpha stays 200
                 GXSetDstAlpha(GX_ENABLE, 90);
                 GXSetAlphaUpdate(GX_FALSE);
                 fill(kQuads[3], 200, 100, 0, 40);
                 GXSetAlphaUpdate(GX_TRUE);
                 GXSetDstAlpha(GX_DISABLE, 0);
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 for (int i = 0; i < 4; ++i) {
                   c.px(img, cx(i), cy(i), 200, 100, 0);
                 }
                 alpha_is(img, c, kPX, kPY, cx(0), cy(0), 160, "constant 160");
                 alpha_is(img, c, kPX, kPY, cx(1), cy(1), 40, "disabled (TEV alpha)");
                 alpha_is(img, c, kPX, kPY, cx(2), cy(2), 0, "constant 0");
                 alpha_is(img, c, kPX, kPY, cx(3), cy(3), 200, "alpha update off");
               }});

  // A colour blend that needs the source alpha cannot also write a constant alpha in one pass (no dual-source blending):
  // draw_op() writes the constant first (colour and depth off), then draws the colours with alpha masked out.
  s.push_back({"dst_alpha_replace_blend",
               "(renderer feature, not live in TP) dst alpha 130 + SRCALPHA colour blends: colour = source-alpha blend, "
               "alpha = 130 (two passes); colour update off = alpha only",
               [] {
                 base_state();
                 rgba6();
                 for (const Quad& q : kQuads) {
                   fill(q, 0, 0, 200, 255);
                 }
                 GXSetDstAlpha(GX_ENABLE, 130);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
                 fill(kQuads[0], 200, 0, 0, 64);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
                 fill(kQuads[1], 200, 0, 0, 64);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR); // no source alpha: a single pass
                 fill(kQuads[2], 200, 0, 0, 64);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
                 GXSetColorUpdate(GX_FALSE); // nothing for the colour blend to do: alpha only
                 fill(kQuads[3], 200, 0, 0, 64);
                 GXSetColorUpdate(GX_TRUE);
                 GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
                 GXSetDstAlpha(GX_DISABLE, 0);
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 const int a = 64;
                 const auto over = [&](int src, int dst) { return static_cast<int>(std::lround(src * a / 255. + dst * (255 - a) / 255.)); };
                 c.px(img, cx(0), cy(0), over(200, 0), 0, over(0, 200));                // SRCALPHA / INVSRCALPHA
                 c.px(img, cx(1), cy(1), static_cast<int>(std::lround(200 * a / 255.)), 0, 200); // SRCALPHA / ONE
                 c.px(img, cx(2), cy(2), 200, 0, 200);                                   // ONE / ONE
                 c.px(img, cx(3), cy(3), 0, 0, 200);                                     // colour update off
                 for (int i = 0; i < 4; ++i) {
                   alpha_is(img, c, kPX, kPY, cx(i), cy(i), 130, "constant alpha");
                 }
               }});

  // GX_BL_DSTALPHA / GX_BL_INVDSTALPHA as colour blend factors (the JPA blend table, JPABaseShape.cpp 1397) read the EFB
  // alpha; the EFB alpha is the 100 / 255 written before. Alpha update off, so the blend does not change it.
  s.push_back({"dst_alpha_blend_factor",
               "RGBA6: blend factors DSTALPHA / INVDSTALPHA weigh by the EFB alpha (100 and 255) and leave it unchanged",
               [] {
                 base_state();
                 rgba6();
                 fill(kQuads[0], 0, 0, 200, 100);
                 fill(kQuads[1], 0, 0, 200, 100);
                 fill(kQuads[2], 0, 0, 200, 100);
                 fill(kQuads[3], 0, 0, 200, 255);
                 GXSetAlphaUpdate(GX_FALSE);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_CLEAR);
                 fill(kQuads[0], 200, 0, 0, 7);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_DSTALPHA, GX_LO_CLEAR);
                 fill(kQuads[1], 200, 0, 0, 7);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ONE, GX_LO_CLEAR);
                 fill(kQuads[2], 200, 0, 0, 7);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_CLEAR);
                 fill(kQuads[3], 200, 0, 0, 7);
                 GXSetAlphaUpdate(GX_TRUE);
                 GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 const auto lerp = [](double w, int src, int dst) { return static_cast<int>(std::lround(src * w + dst * (1 - w))); };
                 const double d = 100 / 255.;
                 c.px(img, cx(0), cy(0), lerp(d, 200, 0), 0, lerp(d, 0, 200));       // src*a + dst*(1-a)
                 c.px(img, cx(1), cy(1), lerp(1 - d, 200, 0), 0, lerp(1 - d, 0, 200)); // src*(1-a) + dst*a
                 c.px(img, cx(2), cy(2), static_cast<int>(std::lround(200 * d)), 0, 200); // src*a + dst
                 c.px(img, cx(3), cy(3), 200, 0, 0);                                    // a = 1: the source
                 alpha_is(img, c, kPX, kPY, cx(0), cy(0), 100, "alpha untouched");
                 alpha_is(img, c, kPX, kPY, cx(3), cy(3), 255, "alpha untouched");
               }});

  // The blend unit applies the colour factors and the equation to alpha too (Aurora: srcAlpha = srcRgb factor, same equation).
  // With alpha update on (preGX's setting) every blended TP draw (2D fades, particles, UI) leaves its blended alpha.
  s.push_back({"alpha_blend_equation",
               "RGBA6, alpha update on: the blend equation also applies to the EFB alpha (SRCALPHA/INVSRCALPHA, ONE/ONE, "
               "SRCALPHA/ONE, subtract)",
               [] {
                 base_state();
                 rgba6();
                 fill(kQuads[0], 0, 0, 200, 200);
                 fill(kQuads[1], 0, 0, 200, 100);
                 fill(kQuads[2], 0, 0, 200, 100);
                 fill(kQuads[3], 0, 0, 200, 200);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
                 fill(kQuads[0], 200, 0, 0, 64);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
                 fill(kQuads[1], 200, 0, 0, 100);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
                 fill(kQuads[2], 200, 0, 0, 64);
                 GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
                 fill(kQuads[3], 50, 0, 120, 90);
                 GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
                 copy_to(0, 0, 0, kN, kN);
                 show_alpha(0, kN, kN, kPX, kPY);
               },
               [](const Image& img, Checker& c) {
                 const double sa = 64 / 255.;
                 const auto r = [](double v) { return static_cast<int>(std::lround(v)); };
                 c.px(img, cx(0), cy(0), r(200 * sa), 0, r(200 * (1 - sa)));
                 c.px(img, cx(1), cy(1), 200, 0, 200);
                 c.px(img, cx(2), cy(2), r(200 * sa), 0, 200);
                 c.px(img, cx(3), cy(3), 0, 0, 80);
                 alpha_is(img, c, kPX, kPY, cx(0), cy(0), r(64 * sa + 200 * (1 - sa)), "SRCALPHA/INVSRCALPHA");
                 alpha_is(img, c, kPX, kPY, cx(1), cy(1), 200, "ONE/ONE");
                 alpha_is(img, c, kPX, kPY, cx(2), cy(2), r(64 * sa + 100), "SRCALPHA/ONE");
                 alpha_is(img, c, kPX, kPY, cx(3), cy(3), 110, "subtract");
               }});

  // Colour and alpha update off, depth on: a depth-only draw (d_a_mirror.cpp 135 draws the mirror plane this way; rain and
  // particle passes at d_kankyo_rain.cpp 5026 / d_particle.cpp 259 switch colour update off too). The rectangle must leave no
  // colour, and still occlude a later, farther rectangle.
  s.push_back({"depth_only_draw",
               "colour + alpha update off, depth test + update on: the near rectangle writes no colour but still hides a "
               "farther one drawn after it",
               [] {
                 base_state();
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
                 GXSetColorUpdate(GX_FALSE);
                 GXSetAlphaUpdate(GX_FALSE);
                 rect(100, 100, 300, 300, 2, 255, 0, 0); // near, depth only
                 GXSetColorUpdate(GX_TRUE);
                 GXSetAlphaUpdate(GX_TRUE);
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
                 rect(0, 150, W, 250, 5, 0, 200, 0); // farther, spans the whole width
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 200, 200, kGray, kGray, kGray); // behind the depth-only rectangle: hidden, and no red
                 c.px(img, 200, 120, kGray, kGray, kGray); // the depth-only rectangle outside the band: no colour
                 c.px(img, 50, 200, 0, 200, 0);            // the far rectangle where nothing hides it
                 c.px(img, 500, 200, 0, 200, 0);
                 c.px(img, 200, 280, kGray, kGray, kGray);
               }});

  // An EFB without alpha (RGB8_Z24, the default): GX_BL_DSTALPHA reads as ONE and GX_BL_INVDSTALPHA as ZERO.
  s.push_back({"dst_alpha_blend_factor_rgb8",
               "RGB8_Z24 (no EFB alpha): DSTALPHA = ONE, INVDSTALPHA = ZERO, whatever alpha was drawn",
               [] {
                 base_state(); // RGB8_Z24
                 fill(kQuads[0], 0, 0, 200, 100);
                 fill(kQuads[1], 0, 0, 200, 100);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_CLEAR);
                 fill(kQuads[0], 200, 0, 0, 7); // src * 1 + dst * 0
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_DSTALPHA, GX_LO_CLEAR);
                 fill(kQuads[1], 200, 0, 0, 7); // src * 0 + dst * 1
               },
               [](const Image& img, Checker& c) {
                 c.px(img, cx(0), cy(0), 200, 0, 0);
                 c.px(img, cx(1), cy(1), 0, 0, 200);
               }});

  // Alpha writes need an alpha EFB: under RGB8_Z24 they are ignored, and a copy is opaque (alpha 255).
  s.push_back({"efb_alpha_rgb8_opaque",
               "alpha 40 written under RGBA6; an RGB8_Z24 overdraw (alpha 10) must not change it, and an RGB8_Z24 copy must "
               "read alpha 255",
               [] {
                 base_state();
                 rgba6();
                 fill(kQuads[0], 20, 40, 60, 40);
                 fill(kQuads[1], 20, 40, 60, 40);
                 GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
                 fill(kQuads[0], 200, 100, 0, 10);
                 fill(kQuads[1], 200, 100, 0, 10);
                 copy_to(0, kQuads[0].x, kQuads[0].y, kQ, kQ); // taken from an RGB8_Z24 EFB: opaque
                 rgba6();
                 copy_to(1, kQuads[1].x, kQuads[1].y, kQ, kQ); // taken from an RGBA6_Z24 EFB: the alpha written under RGBA6
                 show_alpha(0, kQ, kQ, 300, 200);
                 show_alpha(1, kQ, kQ, 400, 200);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, cx(0), cy(0), 200, 100, 0); // colour is written under both formats
                 c.px(img, cx(1), cy(1), 200, 100, 0);
                 alpha_is(img, c, 300, 200, 32, 32, 255, "copy from an RGB8_Z24 EFB");
                 alpha_is(img, c, 400, 200, 32, 32, 40, "RGBA6 alpha survives an RGB8_Z24 overdraw");
               }});
}
