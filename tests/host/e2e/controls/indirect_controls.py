import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

SH = ROOT + '/vita/aurora/overlay/gx/vita_shader.cpp'
ALL = ['tev_ind_warp', 'tev_ind_matrix', 'tev_ind_exp_neg', 'tev_ind_coord_scale', 'tev_ind_itf5', 'tev_ind_itf4',
       'tev_ind_itf3', 'tev_ind_bias_st', 'tev_ind_wrap', 'tev_ind_addprev']
# scenes whose matrix has a non-zero U column (c1.x / c1.y differ) / a non-zero scale exponent / a non-ITF_8 format
U_TERMS = ['tev_ind_matrix', 'tev_ind_exp_neg', 'tev_ind_itf5', 'tev_ind_itf4', 'tev_ind_itf3', 'tev_ind_bias_st']
ITF_N = ['tev_ind_itf5', 'tev_ind_itf4', 'tev_ind_itf3']
ITF_8_BIASED = ['tev_ind_warp', 'tev_ind_matrix', 'tev_ind_exp_neg', 'tev_ind_coord_scale', 'tev_ind_bias_st',
                'tev_ind_wrap', 'tev_ind_addprev']

controls = [
    # ---- shader: the indirect texel ----
    dict(name='shader: S read from R instead of A', file=SH, old='255.0 * {1}.abg', new='255.0 * {1}.rbg', fails=ALL),
    dict(name='shader: T and U swapped (.agb)', file=SH, old='255.0 * {1}.abg', new='255.0 * {1}.agb', fails=ALL),
    dict(name='shader: U read from R instead of G', file=SH, old='255.0 * {1}.abg', new='255.0 * {1}.abr', fails=U_TERMS),
    # ---- shader: format and bias ----
    dict(name='shader: ITF_8 bias -127 instead of -128', file=SH, old='? "-128.0"sv', new='? "-127.0"sv', fails=ITF_8_BIASED),
    dict(name='shader: ITF_5/4/3 bias 0 instead of +1', file=SH, old=': "1.0"sv;', new=': "0.0"sv;', fails=ITF_N),
    dict(name='shader: ITF_5 shifts by 4 instead of 3', file=SH, old='fmtShift = " / 8.0"sv;', new='fmtShift = " / 16.0"sv;',
         fails=['tev_ind_itf5']),
    dict(name='shader: ITF_4 shifts by 3 instead of 4', file=SH, old='fmtShift = " / 16.0"sv;', new='fmtShift = " / 8.0"sv;',
         fails=['tev_ind_itf4']),
    dict(name='shader: ITF_3 shifts by 4 instead of 5', file=SH, old='fmtShift = " / 32.0"sv;', new='fmtShift = " / 16.0"sv;',
         fails=['tev_ind_itf3']),
    dict(name='shader: ITF_5/4/3 without floor (fractional coordinates)', file=SH,
         old='vec3 ind{0}_coord = floor(t_IndTexCoord{1}{2});', new='vec3 ind{0}_coord = (t_IndTexCoord{1}{2});', fails=ITF_N),
    dict(name='shader: ITB_U test replaced by ITB_ST (ITB_ST scene gets a U bias)', file=SH,
         old='stage.indTexBiasSel == GX_ITB_U ||', new='stage.indTexBiasSel == GX_ITB_ST ||', fails=['tev_ind_bias_st']),
    dict(name='shader: S bias dropped', file=SH, old='biasS = "1.0"sv;', new='biasS = "0.0"sv;', fails=ALL),
    dict(name='shader: T bias dropped', file=SH, old='biasT = "1.0"sv;', new='biasT = "0.0"sv;', fails=ALL),
    # ---- shader: matrix ----
    dict(name='shader: row 0 U coefficient taken from c1.y', file=SH, old='ind{0}_c1.x), ind{0}_coord)',
         new='ind{0}_c1.y), ind{0}_coord)', fails=U_TERMS),
    dict(name='shader: matrix rows swapped', file=SH,
         old='"dot(vec3(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "\n              "dot(vec3(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)"',
         new='"dot(vec3(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord), "\n              "dot(vec3(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord)"',
         fails=ALL),
    dict(name='shader: scale exponent not applied', file=SH, old='") * ind{0}_c1.z",', new='")",',
         fails=['tev_ind_matrix', 'tev_ind_exp_neg']),
    dict(name='shader: matrix index fixed to ITM_0 (stage 1 of the add-prev scene uses ITM_1)', file=SH,
         old='const u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;', new='const u32 mtxIdx = 0;', fails=['tev_ind_addprev']),
    dict(name='shader: every stage reads indirect stage 0', file=SH,
         old='"\\n  vec3 ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));',
         new='"\\n  vec3 ind{0}_coord = t_IndTexCoord{1};", i, 0);', fails=['tev_ind_addprev']),
    # ---- shader: indirect coordinate scale ----
    dict(name='shader: ITS s and t swapped', file=SH, old='vec2({2}, {3}) / {4}"', new='vec2({3}, {2}) / {4}"',
         fails=['tev_ind_coord_scale']),
    dict(name='shader: ITS_2 divides by 4', file=SH, old='return "(1.0 / 2.0)"sv;', new='return "(1.0 / 4.0)"sv;',
         fails=['tev_ind_coord_scale']),
    # ---- shader: wrap, add-prev, offset ----
    dict(name='shader: ITW_16 wraps at 32', file=SH, old='size = "16.0"sv;', new='size = "32.0"sv;', fails=['tev_ind_wrap']),
    dict(name='shader: ITW_0 leaves the coordinate alone', file=SH, old='case GX_ITW_0:\n          return "0.0";',
         new='case GX_ITW_0:\n          return coord;', fails=['tev_ind_wrap', 'tev_ind_addprev']),
    dict(name='shader: add-prev overwrites instead of accumulating', file=SH, old='t_TexCoord += {};', new='t_TexCoord = {};',
         fails=['tev_ind_addprev']),
    dict(name='shader: indirect offset not added to the base coordinate', file=SH,
         old='finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetTexel);', new='finalCoord = wrappedExpr;', fails=ALL),
    # ---- upstream GX encode / BP decode / uniform plumbing ----
    dict(name='GXSetIndTexMtx: m[1][2] not written', upstream=('dolphin/gx/GXBump.cpp',
         's32 m12 = static_cast<s32>(1024.0f * mtx[5]) & 0x7FF;', 's32 m12 = 0;'), fails=U_TERMS),
    dict(name='GXSetIndTexMtx: m[1][0] written from mtx[4]', upstream=('dolphin/gx/GXBump.cpp',
         's32 m10 = static_cast<s32>(1024.0f * mtx[3]) & 0x7FF;', 's32 m10 = static_cast<s32>(1024.0f * mtx[4]) & 0x7FF;'),
         fails=ALL),
    dict(name='GXSetIndTexMtx: scale exponent biased by 16 instead of 17', upstream=('dolphin/gx/GXBump.cpp',
         's32 adjScale = scaleExp + 17;', 's32 adjScale = scaleExp + 16;'), fails=ALL),
    dict(name='bp_ind_mtx: row 0 coefficients not sign-extended', upstream=('gx/regs.cpp',
         'packedColumn.x = static_cast<float>(sext11(reg_get(value, 11, 0))) / 1024.0f;',
         'packedColumn.x = static_cast<float>(reg_get(value, 11, 0)) / 1024.0f;'), fails=U_TERMS),
    dict(name='indirect matrix uniform: exp2(scaleExp + 1)', upstream=('gx/shader_info.cpp',
         'std::exp2f(mtx.scaleExp)', 'std::exp2f(mtx.scaleExp + 1)'), fails=ALL),
    dict(name='bp_ind_scale: S and T fields swapped', upstream=('gx/regs.cpp',
         'scaleS = static_cast<GXIndTexScale>(reg_get(value, 4, i * 8));',
         'scaleS = static_cast<GXIndTexScale>(reg_get(value, 4, i * 8 + 4));'), fails=['tev_ind_coord_scale']),
    dict(name='bp_tev_ind: wrap S read from the T field', upstream=('gx/regs.cpp',
         's.indTexWrapS = static_cast<GXIndTexWrap>(reg_get(value, 3, 13));',
         's.indTexWrapS = static_cast<GXIndTexWrap>(reg_get(value, 3, 16));'), fails=['tev_ind_wrap']),
    dict(name='bp_tev_ind: add-prev bit ignored', upstream=('gx/regs.cpp',
         's.indTexAddPrev = reg_get(value, 1, 20) != 0;', 's.indTexAddPrev = false;'), fails=['tev_ind_addprev']),
]

if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
