import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

RP = ROOT + '/vita/aurora/overlay/gfx/vita_gl_replay.cpp'
GX = ROOT + '/vita/aurora/overlay/gx/gx.cpp'
SH = ROOT + '/vita/aurora/overlay/gx/vita_shader.cpp'

AWM, TSP, ECCA, ECA = 'alpha_write_masks', 'tp_shadow_pack', 'efb_copy_clear_alpha', 'efb_clear_alpha'
OB, OD, OR = 'offscreen_basic', 'offscreen_depth_isolation', 'offscreen_reuse_sizes'  # offscreen scenes that also read target alpha / clear
DAR, DARB, DABF, DABF8 = 'dst_alpha_replace', 'dst_alpha_replace_blend', 'dst_alpha_blend_factor', 'dst_alpha_blend_factor_rgb8'
EAR8, ABE, DOD = 'efb_alpha_rgb8_opaque', 'alpha_blend_equation', 'depth_only_draw'

controls = [
    # ---- colour / alpha update ----
    dict(name='replay: alpha write mask always on', file=RP, old='s.colorMask[3] = c.alphaUpdate;', new='s.colorMask[3] = true;',
         fails=[AWM, DAR, DARB, DABF, EAR8]),
    dict(name='replay: colour write mask always on', file=RP,
         old='s.colorMask[0] = s.colorMask[1] = s.colorMask[2] = c.colorUpdate;',
         new='s.colorMask[0] = s.colorMask[1] = s.colorMask[2] = true;', fails=[AWM, DARB, DOD]),
    dict(name='gx.cpp: colour update ignored in the pipeline config', file=GX, old='.colorUpdate = g_gxState.colorUpdate,',
         new='.colorUpdate = true,', fails=[AWM, DARB, DOD]),
    dict(name='gx.cpp: alpha update ignores the GX state (RGBA6 only)', file=GX,
         old='const bool alphaUpdate = hasAlpha && g_gxState.alphaUpdate;', new='const bool alphaUpdate = hasAlpha;',
         fails=[AWM, DAR, DABF]),
    dict(name='upstream gx.hpp: efb_has_alpha() always false', upstream=('gx/gx.hpp',
         'constexpr bool efb_has_alpha(GXPixelFmt format) noexcept { return format == GX_PF_RGBA6_Z24; }',
         'constexpr bool efb_has_alpha(GXPixelFmt format) noexcept { return false && format == GX_PF_RGBA6_Z24; }'),
         fails=[AWM, TSP, OB, ECCA, ECA, DAR, DARB, DABF, EAR8, ABE]),  # OB: a never-drawn target texel must copy out as alpha 0
    dict(name='replay: EFB copy not forced opaque for an RGB8 source', file=RP, old='!gx::efb_has_alpha(r.sourceFormat)',
         new='false', fails=[EAR8]),
    # ---- copy clear ----
    dict(name='replay: copy-clear always clears alpha', file=RP, old='glColorMask(color, color, color, alpha);',
         new='glColorMask(color, color, color, true);', fails=[ECCA]),
    dict(name='replay: copy-clear always clears colour', file=RP, old='glColorMask(color, color, color, alpha);',
         new='glColorMask(true, true, true, alpha);', fails=[ECCA]),
    dict(name='upstream: copy-clear alpha ignores the alpha update bit', upstream=('dolphin/gx/GXFrameBuffer.cpp',
         'const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);',
         'const auto clearAlpha = clear && efb_has_alpha(g_gxState.pixelFmt);'), fails=[ECCA]),
    dict(name='upstream: copy-clear colour ignores the colour update bit', upstream=('dolphin/gx/GXFrameBuffer.cpp',
         'const auto clearColor = clear && g_gxState.colorUpdate;', 'const auto clearColor = clear;'), fails=[ECCA]),
    dict(name='replay: copy-clear of a sub-rectangle clears the whole target', file=RP,
         old='const bool fullTarget = r.rect.x <= 0', new='const bool fullTarget = true || r.rect.x <= 0', fails=[ECCA]),
    dict(name='replay: frame-start clear leaves alpha alone', file=RP,
         old='clear_current(true, true, true, frame.clearColor, frame.clearDepth, true);',
         new='clear_current(true, false, true, frame.clearColor, frame.clearDepth, true);', fails=[ECA]),
    # ---- offscreen target ----
    dict(name='replay: offscreen target cleared to alpha 1', file=RP,
         old='clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 0.f}, 1.f, true);',
         new='clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 1.f}, 1.f, true);', fails=[TSP, OB]),
    dict(name='replay: offscreen target not cleared at begin', file=RP,
         old='      clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 0.f}, 1.f, true);\n', new='',
         # a fresh target is zero-filled on Mesa, so offscreen_basic cannot tell; reuse (OR) and the depth buffer (OD) can
         fails=[TSP, OD, OR]),
    dict(name='replay: render targets sampled nearest (half-size copy = no box average)', file=RP,
         old='void set_linear_clamp(GLuint texture) {\n  glBindTexture(GL_TEXTURE_2D, texture);\n  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);\n  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);',
         new='void set_linear_clamp(GLuint texture) {\n  glBindTexture(GL_TEXTURE_2D, texture);\n  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);\n  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);',
         fails=[TSP]),
    # ---- destination alpha ----
    dict(name='replay: dst alpha uniform = value / 510', file=RP, old='static_cast<float>(draw.dstAlpha) / 255.f',
         new='static_cast<float>(draw.dstAlpha) / 510.f', fails=[DAR, DARB]),
    dict(name='shader: dst alpha replace writes the TEV alpha', file=SH,
         old='gl_FragColor = vec4(prev.rgb, uExtra.x);', new='gl_FragColor = vec4(prev.rgb, prev.a);', fails=[DAR, DARB]),
    dict(name='replay: dst alpha 0 leaves the TEV alpha (blend factor ONE)', file=RP,
         old='  } else if (c.dstAlpha == 0) {\n    d.srcAlpha = GL_ZERO;', new='  } else if (c.dstAlpha == 0) {\n    d.srcAlpha = GL_ONE;',
         fails=[DAR]),
    dict(name='replay: dst alpha + source-alpha blend drawn in one pass', file=RP,
         old='if (dst_alpha_active(*config) && needs_source_alpha(*config)) {',
         new='if (false && dst_alpha_active(*config) && needs_source_alpha(*config)) {', fails=[DARB]),
    dict(name='replay: dst alpha pre-pass also writes colour', file=RP, old='alphaPass.colorUpdate = false;',
         new='alphaPass.colorUpdate = true;', fails=[DARB]),
    dict(name='replay: dst alpha colour pass also writes alpha', file=RP, old='colorPass.alphaUpdate = false;',
         new='colorPass.alphaUpdate = true;', fails=[DARB]),
    # ---- blend factors / equation on alpha ----
    dict(name='replay: GX_BL_DSTALPHA = 1 - dst alpha', file=RP, old='return GL_DST_ALPHA;',
         new='return GL_ONE_MINUS_DST_ALPHA;', fails=[DABF]),
    dict(name='replay: GX_BL_INVDSTALPHA = dst alpha', file=RP, old='return GL_ONE_MINUS_DST_ALPHA;',
         new='return GL_DST_ALPHA;', fails=[DABF]),
    dict(name='gx.cpp: DSTALPHA on an RGB8 EFB reads as ZERO', file=GX,
         old='if (factor == GX_BL_DSTALPHA) {\n        return GX_BL_ONE;', new='if (factor == GX_BL_DSTALPHA) {\n        return GX_BL_ZERO;',
         fails=[DABF8]),
    dict(name='gx.cpp: INVDSTALPHA on an RGB8 EFB reads as ONE', file=GX,
         old='if (factor == GX_BL_INVDSTALPHA) {\n        return GX_BL_ZERO;', new='if (factor == GX_BL_INVDSTALPHA) {\n        return GX_BL_ONE;',
         fails=[DABF8]),
    dict(name='replay: alpha channel not blended (factors ONE/ZERO)', file=RP,
         old='    d.srcAlpha = d.srcRgb;\n    d.dstAlpha = d.dstRgb;', new='    d.srcAlpha = GL_ONE;\n    d.dstAlpha = GL_ZERO;', fails=[ABE]),
    dict(name='replay: GX_BM_SUBTRACT as src - dst', file=RP, old='d.equation = GL_FUNC_REVERSE_SUBTRACT;',
         new='d.equation = GL_FUNC_SUBTRACT;', fails=[ABE]),
]

if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
