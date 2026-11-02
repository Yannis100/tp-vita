// Offscreen render targets as Twilight Princess uses them (included by e2e_main.cpp after scenes_alpha.hpp, whose helpers
// they share).
//
// GXCreateFrameBuffer(w, h) / GXRestoreFrameBuffer() are called from TARGET_PC branches of the game, so they are live on this
// port (the aurora command is GX_AURORA_BEGIN_OFFSCREEN / END_OFFSCREEN; the Vita replay keeps one target per size):
//   d_drawlist.cpp 1593      shadow images: (2*texture)^2 target per batch, additive blend, half-size copy       (tp_shadow_pack)
//   d_map_path.cpp 477       map render: render size rw x rh, logical viewport texW x texH, GXSetViewportRender  (offscreen_map_render)
//   m_Do_graphic.cpp 943     blur pyramid: half-size target, copy each level, sample it back in the same pass    (offscreen_pyramid)
//   m_Do_graphic.cpp 1567    bloom: target of 3/4 x 1/2 of the screen, "oversized" render viewport, sub-rect copies (tp_bloom_layout)
//   m_Do_graphic.cpp 1759    the same bloom, second path
// A target starts as (0,0,0,0) colour and depth 1, has its own depth buffer and its own viewport and scissor (the full target),
// and GXRestoreFrameBuffer() returns to the EFB with the EFB's viewport, scissor and depth as they were.
namespace offscreen {
using namespace alpha;

constexpr int kEfbMarkX = 500; // EFB rectangles drawn before / after an offscreen pass: the pass must not touch the EFB
void efb_proj() {
  f32 proj[4][4];
  ortho(proj, 0.f, H, 0.f, W, 0.1f, 100.f);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
}
void target_proj(int w, int h) {
  f32 proj[4][4];
  ortho(proj, 0.f, static_cast<f32>(h), 0.f, static_cast<f32>(w), 0.1f, 100.f);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
}
// Texel (x, y) of slot `slot` shown magnified by `zoom` at (dx, dy): its centre on the presented frame.
int sx(int dx, int zoom, int x) { return dx + x * zoom + zoom / 2; }
} // namespace offscreen

void add_offscreen_scenes(std::vector<Scene>& s) {
  using namespace offscreen;

  // A 64x48 target (not the EFB's size, not square, not a power of two on the short side): four rectangles, one quadrant
  // left alone. The EFB keeps its own content, and draws after GXRestoreFrameBuffer() use the EFB's viewport and scissor
  // without the game setting them again.
  s.push_back({"offscreen_basic",
               "GXCreateFrameBuffer(64, 48): cleared to (0,0,0,0), three quadrants drawn, copied; the EFB is untouched and "
               "draws after GXRestoreFrameBuffer use its viewport and scissor",
               [] {
                 base_state();
                 rgba6();
                 rect(kEfbMarkX, 50, kEfbMarkX + 60, 110, 1, 255, 0, 255); // before
                 GXCreateFrameBuffer(64, 48);
                 target_proj(64, 48); // the game sets its own projection; viewport and scissor are the target's
                 rect(0, 0, 32, 24, 1, 255, 0, 0);
                 rect(32, 0, 64, 24, 1, 0, 255, 0);
                 rect(0, 24, 32, 48, 1, 0, 0, 255);
                 // A copy larger than its source clamps to the source's edge texels, so a target that is too small would go
                 // unnoticed behind uniform edges: the last columns and rows of the 64x48 target get strips of their own.
                 rect(60, 0, 64, 48, 1, 255, 0, 255);
                 rect(0, 44, 64, 48, 1, 0, 255, 255);
                 copy_to(0, 0, 0, 64, 48);
                 GXRestoreFrameBuffer();
                 efb_proj(); // only the projection: the viewport and scissor must be the EFB's again
                 rect(kEfbMarkX, 150, kEfbMarkX + 60, 210, 1, 0, 255, 255); // after
                 show(0, 64, 48, GX_TF_RGBA8, 100, 200, 2, false);
                 show_alpha(0, 64, 48, 300, 200, GX_TF_RGBA8, 2);
               },
               [](const Image& img, Checker& c) {
                 const auto texel = [&](int dx, int x, int y) { return std::pair{sx(dx, 2, x), sx(200, 2, y)}; };
                 const auto rgb = [&](int x, int y, int r, int g, int b) {
                   const auto [px, py] = texel(100, x, y);
                   c.px(img, px, py, r, g, b);
                 };
                 const auto alpha = [&](int x, int y, int a, const char* what) {
                   const auto [px, py] = texel(300, x, y);
                   alpha_is(img, c, 300, 200, px - 300, py - 200, a, what);
                 };
                 rgb(16, 12, 255, 0, 0);
                 rgb(48, 12, 0, 255, 0);
                 rgb(16, 36, 0, 0, 255);
                 rgb(48, 36, 0, 0, 0); // never drawn: the target's clear colour
                 rgb(0, 0, 255, 0, 0); // the first and last texel of each drawn quadrant
                 rgb(31, 23, 255, 0, 0);
                 rgb(63, 10, 255, 0, 255); // the last column
                 rgb(10, 47, 0, 255, 255); // the last row
                 rgb(63, 47, 0, 255, 255);
                 alpha(16, 12, 255, "drawn");
                 alpha(48, 36, 0, "never drawn (target clear alpha 0)");
                 c.px(img, kEfbMarkX + 30, 80, 255, 0, 255);   // EFB content from before the pass survives
                 c.px(img, kEfbMarkX + 30, 180, 0, 255, 255);  // and the EFB draw after it lands where it was aimed
                 c.px(img, kEfbMarkX + 30, 211, kGray, kGray, kGray); // with the EFB's scissor, not the target's
                 c.px(img, 10, 10, kGray, kGray, kGray);        // nothing of the pass leaked into the EFB's corner
                 c.px(img, 40, 30, kGray, kGray, kGray);
               }});

  // The target has its own depth buffer, and the EFB's depth survives the pass. The EFB holds a near rectangle (z = 2) over
  // the corner; the target's far rectangle (z = 50) at the same pixels must still be drawn (fresh depth 1.0), and a mid
  // rectangle (z = 20) drawn on the EFB afterwards must still be hidden by the near one.
  s.push_back({"offscreen_depth_isolation",
               "own depth buffer: a far rectangle in the target is not hidden by the EFB's near one at the same pixels; the "
               "EFB's depth is intact after the pass",
               [] {
                 base_state();
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
                 rect(0, 0, 128, 128, 2, 255, 0, 0); // EFB: near, red
                 GXCreateFrameBuffer(128, 128);
                 target_proj(128, 128);
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
                 rect(0, 0, 128, 128, 50, 0, 255, 0); // target: far, green
                 copy_to(0, 0, 0, 128, 128);
                 GXRestoreFrameBuffer();
                 efb_proj();
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
                 rect(0, 0, 128, 128, 20, 0, 0, 255); // EFB: mid, blue: behind the near rectangle
                 GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_TRUE);
                 show(0, 128, 128, GX_TF_RGBA8, 300, 200, 1, false);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 64, 64, 255, 0, 0);          // the near rectangle still wins on the EFB
                 c.px(img, 300 + 64, 200 + 64, 0, 255, 0); // the target's own draw was not depth-tested against it
               }});

  // Targets are kept per size: re-creating one clears it, a different size is a different target, and a target at least as tall
  // and wide as the request is not good enough (a copy larger than its source clamps to the source's edge texels, so the pictures
  // carry distinct bands in their last rows and columns). Pass B is the short one (64x32), A and C are 64x64; A leaves a small
  // viewport and scissor behind, which creating the target again for C must reset.
  s.push_back({"offscreen_reuse_sizes",
               "targets 64x32, 64x64, 64x64 in one frame: each copy holds only its own pass, the 64x64 target is as tall as "
               "asked, the second 64x64 pass is cleared and has the full viewport and scissor",
               [] {
                 base_state();
                 GXCreateFrameBuffer(64, 32);
                 target_proj(64, 32);
                 rect(0, 0, 60, 16, 1, 0, 255, 0); // pass B: green top left, a magenta last-column strip, black elsewhere
                 rect(60, 0, 64, 32, 1, 255, 0, 255);
                 copy_to(1, 0, 0, 64, 32);
                 GXCreateFrameBuffer(64, 64);
                 target_proj(64, 64);
                 rect(0, 0, 64, 32, 1, 255, 0, 0); // pass A: red / blue / yellow bands, the yellow one in rows 48..63
                 rect(0, 32, 64, 48, 1, 0, 0, 255);
                 rect(0, 48, 64, 64, 1, 255, 255, 0);
                 copy_to(0, 0, 0, 64, 64);
                 GXSetViewport(0.f, 0.f, 32.f, 32.f, 0.f, 1.f); // left behind on this 64x64 target for its next pass:
                 GXSetScissor(0, 0, 32, 32);                    // creating it again must reset both
                 GXCreateFrameBuffer(64, 64);
                 target_proj(64, 64);
                 rect(0, 0, 16, 16, 1, 0, 0, 255); // pass C: a blue corner and a green square in rows 50..61, nothing else
                 rect(40, 50, 60, 62, 1, 0, 255, 0);
                 copy_to(2, 0, 0, 64, 64);
                 GXRestoreFrameBuffer();
                 base_state();
                 show(0, 64, 64, GX_TF_RGBA8, 100, 200, 1, false);
                 show(1, 64, 32, GX_TF_RGBA8, 200, 200, 1, false);
                 show(2, 64, 64, GX_TF_RGBA8, 300, 200, 1, false);
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 100 + 32, 200 + 16, 255, 0, 0); // A: three bands
                 c.px(img, 100 + 32, 200 + 40, 0, 0, 255);
                 c.px(img, 100 + 32, 200 + 56, 255, 255, 0);
                 c.px(img, 100 + 2, 200 + 62, 255, 255, 0);
                 c.px(img, 200 + 30, 200 + 8, 0, 255, 0);  // B: green top left, magenta strip, black below the green
                 c.px(img, 200 + 62, 200 + 24, 255, 0, 255);
                 c.px(img, 200 + 30, 200 + 24, 0, 0, 0);
                 c.px(img, 300 + 12, 200 + 12, 0, 0, 255); // C: blue corner (16 px: the full viewport), green square (the
                 c.px(img, 300 + 50, 200 + 56, 0, 255, 0); // full scissor), black everywhere else, nothing left from A
                 c.px(img, 300 + 30, 200 + 30, 0, 0, 0);
                 c.px(img, 300 + 50, 200 + 40, 0, 0, 0);
                 c.px(img, 300 + 8, 200 + 40, 0, 0, 0);
                 c.px(img, 300 + 32, 200 + 16, 0, 0, 0);
                 c.px(img, 300 + 32, 200 + 56, 0, 0, 0);
               }});

  // d_map_path.cpp dRenderingFDAmap_c::preRenderingMap (TARGET_PC): the target is the render size (rw x rh), the logical
  // viewport and scissor stay texW x texH, and GXSetViewportRender / GXSetScissorRender make the GL viewport the render size.
  // Here logical 64x48, render 128x96 (a 2x map): the same ortho world fills the whole render target, and line width and point
  // size grow with render / logical.
  s.push_back({"offscreen_map_render",
               "(d_map_path.cpp 477) target 128x96, logical viewport 64x48, GXSetViewportRender 128x96: the world fills the "
               "target and a 2-logical-pixel line is 4 render pixels wide",
               [] {
                 base_state();
                 GXCreateFrameBuffer(128, 96);
                 GXSetViewport(0.f, 0.f, 64.f, 48.f, 0.f, 1.f);
                 GXSetScissor(0, 0, 64, 48);
                 GXSetViewportRender(0.f, 0.f, 128.f, 96.f, 0.f, 1.f);
                 GXSetScissorRender(0, 0, 128, 96);
                 target_proj(64, 48); // the world is the logical 64x48
                 rect(0, 0, 32, 24, 1, 255, 0, 0);
                 rect(32, 0, 64, 24, 1, 0, 255, 0);
                 rect(0, 24, 32, 48, 1, 0, 0, 255);
                 rect(32, 24, 64, 48, 1, 255, 255, 0);
                 rect(62, 0, 64, 48, 1, 255, 255, 255); // the last 4 render columns: a target that is too narrow loses them
                 GXSetLineWidth(12, GX_TO_ZERO); // 12/6 = 2 logical pixels
                 GXBegin(GX_LINES, GX_VTXFMT0, 2);
                 GXPosition3f32(8.f, 12.f, -1.f);
                 GXColor4u8(255, 255, 255, 255);
                 GXPosition3f32(56.f, 12.f, -1.f);
                 GXColor4u8(255, 255, 255, 255);
                 GXEnd();
                 copy_to(0, 0, 0, 128, 96);
                 GXRestoreFrameBuffer();
                 base_state();
                 show(0, 128, 96, GX_TF_RGBA8, 100, 200, 1, false);
               },
               [](const Image& img, Checker& c) {
                 // quadrants at render scale: each 64x48 render pixels
                 c.px(img, 100 + 32, 200 + 40, 255, 0, 0);
                 c.px(img, 100 + 96, 200 + 40, 0, 255, 0);
                 c.px(img, 100 + 32, 200 + 60, 0, 0, 255);
                 c.px(img, 100 + 96, 200 + 80, 255, 255, 0);
                 c.px(img, 100 + 63, 200 + 47, 255, 0, 0); // quadrant corner
                 c.px(img, 100 + 64, 200 + 48, 255, 255, 0);
                 c.px(img, 100 + 126, 200 + 10, 255, 255, 255); // the right-edge marker, top and bottom
                 c.px(img, 100 + 126, 200 + 90, 255, 255, 255);
                 c.px(img, 100 + 123, 200 + 90, 255, 255, 0);
                 // the line: logical y = 12 -> render y = 24, 2 logical px = 4 render px wide (rows 22..25), x 16 .. 112
                 for (const int x : {40, 100}) { // one pixel above and below the line is still the quadrant colour
                   for (const int y : {22, 23, 24, 25}) {
                     c.px(img, 100 + x, 200 + y, 255, 255, 255, 3);
                   }
                   c.px(img, 100 + x, 200 + 21, x < 64 ? 255 : 0, x < 64 ? 0 : 255, 0, 3);
                   c.px(img, 100 + x, 200 + 26, x < 64 ? 255 : 0, x < 64 ? 0 : 255, 0, 3);
                 }
               }});

  // m_Do_graphic.cpp bloom_c::draw2: target (w/2 + w/4) x (h/2) for a w x h screen, GXSetViewportRender(0, 0, w, h) "oversized",
  // quads placed with normalised ortho coordinates at divRects[i] (level 1 at (0,0,w/2,h/2), level 2 at (w/2,0,w/4,h/4),
  // level 3 below it), and a GXSetTexCopySrc of each rectangle. Here w x h = 128 x 96: target 96 x 48.
  s.push_back({"tp_bloom_layout",
               "(m_Do_graphic.cpp 1567) target 96x48 with an oversized render viewport 128x96: level quads land at their "
               "divRects, each rectangle copies on its own, the rest stays (0,0,0,0)",
               [] {
                 constexpr int w = 128, h = 96;
                 struct R {
                   int x, y, w, h;
                 };
                 const R div[4] = {{0, 0, w, h}, {0, 0, w / 2, h / 2}, {w / 2, 0, w / 4, h / 4}, {w / 2, h / 4, w / 8, h / 8}};
                 base_state();
                 GXCreateFrameBuffer(div[2].x + div[2].w, div[1].y + div[1].h);
                 GXSetViewportRender(0.f, 0.f, w, h, 0.f, 1.f);
                 f32 proj[4][4];
                 ortho(proj, 0.f, 1.f, 0.f, 1.f, 0.f, 10.f); // C_MTXOrtho(ortho, 0, 1, 0, 1, 0, 10)
                 GXSetProjection(proj, GX_ORTHOGRAPHIC);
                 const u8 colours[4][3] = {{90, 90, 90}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
                 for (int i = 3; i >= 1; --i) { // level 0 would cover everything; the game draws it on the EFB
                   const R& r = div[i];
                   rect(static_cast<f32>(r.x) / w, static_cast<f32>(r.y) / h, static_cast<f32>(r.x + r.w) / w,
                        static_cast<f32>(r.y + r.h) / h, 5, colours[i][0], colours[i][1], colours[i][2]);
                 }
                 for (int i = 1; i <= 3; ++i) {
                   copy_to(i - 1, div[i].x, div[i].y, div[i].w, div[i].h);
                 }
                 copy_to(3, 0, 0, 96, 48);
                 GXRestoreFrameBuffer();
                 base_state();
                 show(0, 64, 48, GX_TF_RGBA8, 20, 200, 1, false);  // level 1
                 show(1, 32, 24, GX_TF_RGBA8, 100, 200, 1, false); // level 2
                 show(2, 16, 12, GX_TF_RGBA8, 150, 200, 1, false); // level 3
                 show(3, 96, 48, GX_TF_RGBA8, 200, 200, 1, false); // the whole target
               },
               [](const Image& img, Checker& c) {
                 c.px(img, 20, 200, 255, 0, 0);  // level 1 copy: red from its first texel to its last
                 c.px(img, 20 + 63, 200 + 47, 255, 0, 0);
                 c.px(img, 20 + 32, 200 + 24, 255, 0, 0);
                 c.px(img, 100, 200, 0, 255, 0); // level 2: green only
                 c.px(img, 100 + 31, 200 + 23, 0, 255, 0);
                 c.px(img, 150, 200, 0, 0, 255); // level 3: blue only
                 c.px(img, 150 + 15, 200 + 11, 0, 0, 255);
                 // the whole target: level 1 | level 2 over level 3, black where no level was drawn (x 80..95, y 36..47)
                 c.px(img, 200 + 10, 200 + 10, 255, 0, 0);
                 c.px(img, 200 + 70, 200 + 10, 0, 255, 0);
                 c.px(img, 200 + 70, 200 + 30, 0, 0, 255);
                 c.px(img, 200 + 90, 200 + 40, 0, 0, 0);
                 c.px(img, 200 + 90, 200 + 30, 0, 0, 0);
               }});

  // m_Do_graphic.cpp 943: a pyramid inside one offscreen pass. Each level is copied (half size, a box average) into a texture
  // and that texture is drawn back into the same target one level down. The copies are read from the target, not from the
  // EFB, and a texture copied in this pass is sampled while drawing into the target it came from.
  s.push_back({"offscreen_pyramid",
               "(m_Do_graphic.cpp 943) copy the target to half size, draw that copy into the same target at half size, repeat: "
               "the copies come from the offscreen target and sampling them while drawing is safe",
               [] {
                 base_state();
                 GXCreateFrameBuffer(64, 64);
                 target_proj(64, 64);
                 rect(0, 0, 32, 32, 1, 255, 0, 0);
                 rect(32, 0, 64, 32, 1, 0, 255, 0);
                 rect(0, 32, 32, 64, 1, 0, 0, 255);
                 rect(32, 32, 64, 64, 1, 255, 255, 0);
                 // level 1: the game copies to a half-size texture (source 64x64, destination 32x32)
                 GXSetTexCopySrc(0, 0, 64, 64);
                 GXSetTexCopyDst(32, 32, GX_TF_RGBA8, GX_FALSE);
                 GXCopyTex(gCopy[2], GX_FALSE);
                 // draw it back into the top-left 32x32
                 GXInitTexObj(&gObj[2], gCopy[2], 32, 32, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
                 GXInitTexObjLOD(&gObj[2], GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
                 GXLoadTexObj(&gObj[2], GX_TEXMAP0);
                 GXSetNumTexGens(1);
                 GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
                 GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
                 GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
                 GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
                 tex_rect(0, 0, 32, 32, 1);
                 // level 2: copy that 32x32 corner to 16x16 and draw it into the top-left 16x16
                 GXSetTexCopySrc(0, 0, 32, 32);
                 GXSetTexCopyDst(16, 16, GX_TF_RGBA8, GX_FALSE);
                 GXCopyTex(gCopy[3], GX_FALSE);
                 GXInitTexObj(&gObj[3], gCopy[3], 16, 16, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
                 GXInitTexObjLOD(&gObj[3], GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
                 GXLoadTexObj(&gObj[3], GX_TEXMAP0);
                 tex_rect(0, 0, 16, 16, 1);
                 copy_to(0, 0, 0, 64, 64);
                 GXRestoreFrameBuffer();
                 base_state();
                 show(0, 64, 64, GX_TF_RGBA8, 100, 200, 2, false);
               },
               [](const Image& img, Checker& c) {
                 const auto at = [&](int x, int y, int r, int g, int b) { c.px(img, 100 + 2 * x + 1, 200 + 2 * y + 1, r, g, b); };
                 // level 2 (16x16, quadrants of 8): top-left corner
                 at(4, 4, 255, 0, 0);
                 at(12, 4, 0, 255, 0);
                 at(4, 12, 0, 0, 255);
                 at(12, 12, 255, 255, 0);
                 // level 1 (32x32, quadrants of 16), outside level 2
                 at(24, 8, 0, 255, 0);
                 at(8, 24, 0, 0, 255);
                 at(24, 24, 255, 255, 0);
                 // level 0 outside level 1
                 at(48, 16, 0, 255, 0);
                 at(16, 48, 0, 0, 255);
                 at(48, 48, 255, 255, 0);
                 at(40, 4, 0, 255, 0);
               }});
}
