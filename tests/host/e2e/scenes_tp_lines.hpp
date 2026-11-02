// Line and point draws as Twilight Princess issues them (included by e2e_main.cpp; the helpers live there).
//
// GX_LINES / GX_LINESTRIP / GX_POINTS are expanded on the CPU into screen-space quads, and the vertex shader of such a draw
// reads three extra attributes (aCorner, aPosOther, aPnMtxIdxOther). The shader generator's fuzz reaches 19 vertex
// attributes on these draws, over the 16 vitaGL / GXM allow. These scenes issue the GX calls of every line / point site the
// game has (call sequences copied from the source named in each scene) and pin, besides the pixels, how many attributes the
// draw needs:
//
//   decoded   = 1 (aPnMtxIdx, always) + one per vertex-descriptor attribute + aCorner (+ aPosOther, aPnMtxIdxOther for lines)
//   declared  = the attributes the generated vertex shader uses: decoded minus the vertex-descriptor attributes no TEV stage
//               or channel reads (a texcoord no stage samples is not declared)
//
// Sites (all GXBegin(GX_LINES / GX_LINESTRIP / GX_POINTS) in src/ and libs/, no game data involved):
//   m_Do_ext.cpp 3387/3442/3501/3551 (line/point packets), d_drawlist.cpp 987 (effect lines)   POS only          decoded 5 / 3
//   d_drawlist.cpp 272 (dDlst_2DPoint)                                                         POS only (S16)   decoded 3
//   J2DGrafContext.cpp 166/187, JAWGraphContext.cpp 126/150, HBMGUIManager.cpp 46              POS + CLR0       decoded 6
//   d_map_path.cpp 293, d_map.cpp 457/492/1035/1056                                            POS (+ TEX0)     decoded 5..6
//   d_map_path.cpp 568/581/596/608 (map decoration lines and points)                           POS idx + TEX0   decoded 6 / 4
//   JPABaseShape.cpp 1034/1061 (JPADrawPoint / JPADrawLine)                                    POS + TEX0       decoded 4 / 6
// The largest descriptor on any of them is two attributes besides the matrix index, so no TP line or point draw needs more
// than 6 of the 16.

// An ortho projection whose z range contains 0 (J2D / map code draws at z = 0; base_state's near plane is at 0.1).
void ortho_z0() {
  f32 proj[4][4];
  ortho(proj, 0.f, H, 0.f, W, -1.f, 1.f);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
}

// Source-over blend of colour channel `c` with alpha `a` (0..255) onto the harness's gray clear colour.
int over_gray(int c, int a) { return static_cast<int>(std::lround(c * (a / 255.0) + kGray * (1.0 - a / 255.0))); }

void add_tp_line_scenes(std::vector<Scene>& s) {
  // m_Do_ext.cpp mDoExt_line_class::draw: POS only, colour from TEV register C0 / alpha from A0, blended.
  s.push_back({"tp_line_ext",
               "(m_Do_ext.cpp) GX_LINES, descriptor POS only, colour from TEVREG0 over SRCALPHA/INVSRCALPHA: 4 px line, "
               "decoded 5 / declared 5 vertex attributes",
               [] {
                 base_state();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
                 GXSetCurrentMtx(0);
                 GXSetNumChans(1);
                 GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
                 GXSetNumTexGens(0);
                 GXSetNumTevStages(1);
                 GXSetTevColor(GX_TEVREG0, GXColor{200, 60, 20, 128});
                 GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                 GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
                 GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
                 GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
                 GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
                 GXSetZCompLoc(GX_ENABLE);
                 GXSetZMode(GX_DISABLE, GX_LEQUAL, GX_DISABLE);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
                 GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
                 GXSetCullMode(GX_CULL_NONE);
                 GXSetNumIndStages(0);
                 GXSetLineWidth(24, GX_TO_ZERO);
                 GXBegin(GX_LINES, GX_VTXFMT0, 2);
                 GXPosition3f32(100, 150, -1.f);
                 GXPosition3f32(500, 150, -1.f);
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 const int r = over_gray(200, 128), g = over_gray(60, 128), b = over_gray(20, 128);
                 for (const int y : {148, 149, 150, 151}) c.px(img, 300, y, r, g, b, 2);
                 c.px(img, 300, 146, kGray, kGray, kGray);
                 c.px(img, 300, 153, kGray, kGray, kGray);
                 c.px(img, 104, 150, r, g, b, 2);
                 c.px(img, 496, 150, r, g, b, 2);
                 c.px(img, 96, 150, kGray, kGray, kGray);
                 c.px(img, 504, 150, kGray, kGray, kGray);
                 c.attrs(5, 5);
               }});

  // d_drawlist.cpp dDlst_2DPoint::draw: POS only with 16-bit positions, colour from the channel's material register.
  s.push_back({"tp_point_2d",
               "(d_drawlist.cpp) GX_POINTS, POS (S16) only, colour from the channel material register, blended: two 8x8 px "
               "points, one with a negative coordinate moved by the position matrix; decoded 3 / declared 3",
               [] {
                 base_state();
                 ortho_z0();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
                 GXSetNumChans(1);
                 GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
                 GXSetChanMatColor(GX_COLOR0A0, GXColor{255, 128, 0, 200});
                 GXSetNumTexGens(0);
                 GXSetNumTevStages(1);
                 GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                 GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
                 GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
                 GXSetPointSize(48, GX_TO_ZERO);
                 f32 mtx[3][4];
                 ident(mtx);
                 mtx[0][3] = 300.f; // vertices at -100 and +100 land at x = 200 and 400
                 GXLoadPosMtxImm(mtx, GX_PNMTX0);
                 GXSetCurrentMtx(GX_PNMTX0);
                 GXBegin(GX_POINTS, GX_VTXFMT0, 2);
                 GXPosition3s16(-100, 250, 0);
                 GXPosition3s16(100, 250, 0);
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 const int r = over_gray(255, 200), g = over_gray(128, 200), b = over_gray(0, 200);
                 for (const int cx : {200, 400}) {
                   for (const int x : {cx - 4, cx, cx + 3}) {
                     for (const int y : {246, 250, 253}) c.px(img, x, y, r, g, b, 2);
                   }
                   c.px(img, cx - 5, 250, kGray, kGray, kGray);
                   c.px(img, cx + 4, 250, kGray, kGray, kGray);
                   c.px(img, cx, 245, kGray, kGray, kGray);
                   c.px(img, cx, 254, kGray, kGray, kGray);
                 }
                 c.px(img, 300, 250, kGray, kGray, kGray);
                 c.attrs(3, 3);
               }});

  // J2DGrafContext.cpp drawFrame (also JAWGraphContext / HBM lines): POS + CLR0, vertex colour interpolated along the line.
  s.push_back({"tp_line_j2d",
               "(J2DGrafContext.cpp) GX_LINESTRIP box outline, POS + CLR0: colour interpolates between the corner colours, "
               "decoded 6 / declared 6",
               [] {
                 base_state();
                 ortho_z0();
                 GXClearVtxDesc();
                 GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
                 GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
                 GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
                 GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
                 GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_GREATER, 0);
                 GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
                 GXSetLineWidth(12, GX_TO_ZERO);
                 struct Corner {
                   f32 x, y;
                   u8 r, g, b;
                 };
                 const Corner box[5] = {{100, 100, 255, 0, 0}, {400, 100, 0, 255, 0}, {400, 300, 0, 0, 255},
                                        {100, 300, 255, 255, 0}, {100, 100, 255, 0, 0}};
                 GXBegin(GX_LINESTRIP, GX_VTXFMT0, 5);
                 for (const Corner& v : box) {
                   GXPosition3f32(v.x, v.y, 0.f);
                   GXColor4u8(v.r, v.g, v.b, 255);
                 }
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 const auto lerp = [](const int a[3], const int b[3], double t, int k) { return static_cast<int>(std::lround(a[k] + (b[k] - a[k]) * t)); };
                 const int tl[3] = {255, 0, 0}, tr[3] = {0, 255, 0}, br[3] = {0, 0, 255}, bl[3] = {255, 255, 0};
                 // Top edge (rows 99..100), x runs 100 -> 400.
                 for (const int x : {150, 250, 350}) {
                   const double t = (x + 0.5 - 100.0) / 300.0;
                   c.px(img, x, 99, lerp(tl, tr, t, 0), lerp(tl, tr, t, 1), lerp(tl, tr, t, 2), 4);
                   c.px(img, x, 100, lerp(tl, tr, t, 0), lerp(tl, tr, t, 1), lerp(tl, tr, t, 2), 4);
                 }
                 // Right edge (columns 399..400), y runs 100 -> 300.
                 for (const int y : {150, 200, 250}) {
                   const double t = (y + 0.5 - 100.0) / 200.0;
                   c.px(img, 399, y, lerp(tr, br, t, 0), lerp(tr, br, t, 1), lerp(tr, br, t, 2), 4);
                   c.px(img, 400, y, lerp(tr, br, t, 0), lerp(tr, br, t, 1), lerp(tr, br, t, 2), 4);
                 }
                 // Bottom edge (rows 299..300), x runs 400 -> 100.
                 for (const int x : {150, 250, 350}) {
                   const double t = (400.0 - (x + 0.5)) / 300.0;
                   c.px(img, x, 299, lerp(br, bl, t, 0), lerp(br, bl, t, 1), lerp(br, bl, t, 2), 4);
                 }
                 // Left edge (columns 99..100), y runs 300 -> 100.
                 for (const int y : {150, 200, 250}) {
                   const double t = (300.0 - (y + 0.5)) / 200.0;
                   c.px(img, 100, y, lerp(bl, tl, t, 0), lerp(bl, tl, t, 1), lerp(bl, tl, t, 2), 4);
                 }
                 c.px(img, 250, 200, kGray, kGray, kGray); // inside the box
                 c.px(img, 250, 95, kGray, kGray, kGray);  // outside
                 // Thickness 12 / 6 = 2 px: the rows / columns next to the edges are untouched.
                 c.px(img, 250, 98, kGray, kGray, kGray);
                 c.px(img, 250, 101, kGray, kGray, kGray);
                 c.px(img, 98, 200, kGray, kGray, kGray);
                 c.px(img, 101, 200, kGray, kGray, kGray);
                 c.px(img, 398, 200, kGray, kGray, kGray);
                 c.px(img, 401, 200, kGray, kGray, kGray);
                 c.attrs(6, 6);
               }});

  // d_map_path.cpp renderingDecoration: positions are 16-bit indices into a position array (INDEX16, 2 floats), TEX0 direct but
  // not read by the line pass (TEV: C0 colour, KONST alpha).
  static uint32_t sMapPos[3][2];
  static const u16 sMapIdx[3] = {0, 1, 2};
  const auto map_state = [](GXColor c0, GXColor c1) {
    base_state();
    ortho_z0();
    build_test_texture();
    // setTevSettingIntensityTextureToCI(), then GXSetNumTevStages(1)
    GXSetNumTevStages(2);
    GXSetNumChans(1);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 60);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_C1);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_C2, GX_CC_CPREV, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_1_4);
    // the decoration pass
    const f32 pts[3][2] = {{120, 380}, {260, 430}, {400, 380}};
    for (int i = 0; i < 3; ++i) {
      sMapPos[i][0] = be32(pts[i][0]);
      sMapPos[i][1] = be32(pts[i][1]);
    }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSETARRAY(GX_VA_POS, sMapPos, sizeof(sMapPos), 8, false);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
    GXSetNumTevStages(1);
    static GXTexObj obj;
    GXInitTexObj(&obj, gTexRgba8, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    GXSetLineWidth(36, GX_TO_ONE);
    GXSetPointSize(60, GX_TO_ONE);
    GXSetTevColor(GX_TEVREG0, c0);
    GXSetTevColor(GX_TEVREG1, c1);
  };
  const auto map_lines = [map_state] {
    map_state(GXColor{30, 160, 90, 255}, GXColor{100, 150, 50, 255});
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    // TP emits each segment as a pair of vertices: 2 * (n - 1) vertices for n points.
    GXBegin(GX_LINESTRIP, GX_VTXFMT0, 4);
    for (int i = 0; i < 2; ++i) {
      GXPosition1x16(sMapIdx[i]);
      GXTexCoord2f32(0, 0);
      GXPosition1x16(sMapIdx[i + 1]);
      GXTexCoord2f32(0, 0);
    }
    GXEnd();
  };
  s.push_back({"tp_map_path_line",
               "(d_map_path.cpp) GX_LINESTRIP over indexed POS_XY, TEX0 direct but unread (C0 colour, KONST alpha), "
               "6 px lines: decoded 6 / declared 5 (aTex0 is not used by the TEV)",
               map_lines,
               [](const Image& img, Checker& c) {
                 // Segment 1: (120,380) -> (260,430); segment 2: (260,430) -> (400,380). Midpoints are on the lines.
                 c.px(img, 190, 405, 30, 160, 90, 2);
                 c.px(img, 330, 405, 30, 160, 90, 2);
                 c.px(img, 160, 394, 30, 160, 90, 2); // x = 160 is 40 / 140 along segment 1: y = 380 + 50 * 40 / 140 = 394.3
                 // 6 px thick: 2 px off the centre line is covered, 4.4 px is not (segment 1 is 20 degrees off horizontal).
                 c.px(img, 190, 407, 30, 160, 90, 2);
                 c.px(img, 189, 409, kGray, kGray, kGray);
                 // Well off the lines, including above the apex where a degenerate (zero length) segment would smear.
                 c.px(img, 190, 380, kGray, kGray, kGray);
                 c.px(img, 260, 400, kGray, kGray, kGray);
                 c.px(img, 260, 460, kGray, kGray, kGray);
                 c.px(img, 330, 380, kGray, kGray, kGray);
                 c.attrs(6, 5);
               }});
  s.push_back({"tp_map_path_point",
               "(d_map_path.cpp) GX_POINTS over indexed POS_XY + TEX0, colour C1 + KONST(1/4) * texel(0,0): "
               "10 px points, decoded 4 / declared 4",
               [map_state] {
                 map_state(GXColor{30, 160, 90, 255}, GXColor{100, 150, 50, 255});
                 GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_C1);
                 GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                 GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
                 GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                 GXBegin(GX_POINTS, GX_VTXFMT0, 3);
                 for (const u16 idx : sMapIdx) {
                   GXPosition1x16(idx);
                   GXTexCoord2f32(0, 0);
                 }
                 GXEnd();
               },
               [](const Image& img, Checker& c) {
                 // texel (0, 0) = (40, 40, 30); out = C1 + 0.25 * texel.
                 const int r = 100 + 10, g = 150 + 10, b = 50 + 8;
                 for (const auto& [x, y] : {std::pair{120, 380}, {260, 430}, {400, 380}}) {
                   c.px(img, x, y, r, g, b, 3);
                   c.px(img, x - 4, y + 4, r, g, b, 3);
                   c.px(img, x + 4, y - 4, r, g, b, 3);
                   c.px(img, x + 8, y, kGray, kGray, kGray);
                   c.px(img, x, y - 8, kGray, kGray, kGray);
                 }
                 c.attrs(4, 4);
               },
               map_lines}); // the map code draws every frame with the same position array: the second frame must still see it

  // JPABaseShape.cpp JPADrawLine / JPADrawPoint: POS + TEX0 direct (VTXFMT1), texture * TEVREG0, the texcoord runs along the line.
  const auto jpa_state = [] {
    textured_state(gTexRgba8, 4, 4, GX_TF_RGBA8, GX_MODULATE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumChans(0);
    GXSetTevColor(GX_TEVREG0, GXColor{200, 255, 100, 255});
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetLineWidth(36, GX_TO_ZERO);
    GXSetPointSize(60, GX_TO_ZERO);
  };
  const auto tinted = [](int x, int y) { // texel (x, y) of the test texture times C0 = (200, 255, 100)
    const auto t = test_texel(x, y);
    return std::array<int, 3>{static_cast<int>(std::lround(t[0] * 200 / 255.0)), t[1], static_cast<int>(std::lround(t[2] * 100 / 255.0))};
  };
  s.push_back({"tp_jpa_line",
               "(JPABaseShape.cpp JPADrawLine) GX_LINES, POS + TEX0 (VTXFMT1), texture x C0, texcoord t runs 0 -> 1 along the line: "
               "decoded 6 / declared 6",
               [jpa_state] {
                 jpa_state();
                 GXBegin(GX_LINES, GX_VTXFMT1, 2);
                 GXPosition3f32(100, 300, -1.f);
                 GXTexCoord2f32(0.f, 0.f);
                 GXPosition3f32(500, 300, -1.f);
                 GXTexCoord2f32(0.f, 1.f);
                 GXEnd();
               },
               [tinted](const Image& img, Checker& c) {
                 // t = (x + 0.5 - 100) / 400 selects texel row floor(4 t) of column 0; the middle of each row's span.
                 for (int row = 0; row < 4; ++row) {
                   const int x = 150 + 100 * row;
                   const auto e = tinted(0, row);
                   c.px(img, x, 299, e[0], e[1], e[2], 3);
                   c.px(img, x, 301, e[0], e[1], e[2], 3);
                 }
                 c.px(img, 250, 296, kGray, kGray, kGray); // 36 / 6 = 6 px thick: rows 297..302
                 c.px(img, 250, 304, kGray, kGray, kGray);
                 c.attrs(6, 6);
               }});
  s.push_back({"tp_jpa_point",
               "(JPABaseShape.cpp JPADrawPoint) GX_POINTS, POS + TEX0 (VTXFMT1), texture x C0 at texcoord (0,0) = texel (0,0): "
               "10 px point, decoded 4 / declared 4",
               [jpa_state] {
                 jpa_state();
                 GXBegin(GX_POINTS, GX_VTXFMT1, 1);
                 GXPosition3f32(300, 300, -1.f);
                 GXTexCoord2f32(0.f, 0.f);
                 GXEnd();
               },
               [tinted](const Image& img, Checker& c) {
                 const auto e = tinted(0, 0);
                 c.px(img, 300, 300, e[0], e[1], e[2], 3);
                 c.px(img, 296, 304, e[0], e[1], e[2], 3);
                 c.px(img, 304, 296, e[0], e[1], e[2], 3);
                 c.px(img, 308, 300, kGray, kGray, kGray); // 60 / 6 = 10 px: columns / rows 295..304
                 c.px(img, 300, 292, kGray, kGray, kGray);
                 c.attrs(4, 4);
               }});
}
