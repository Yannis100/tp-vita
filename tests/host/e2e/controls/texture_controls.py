import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

CONV = ROOT + '/vita/aurora/overlay/gfx/vita_texture_convert.cpp'
TEX = ROOT + '/vita/aurora/overlay/gfx/vita_texture.cpp'
TEXGX = ROOT + '/vita/aurora/overlay/gx/texture.cpp'
REPLAY = ROOT + '/vita/aurora/overlay/gfx/vita_gl_replay.cpp'

RGB5A3_TLUT = ['tex_pal_c8_rgb5a3', 'tex_pal_two_tluts', 'tex_pal_efb_copy', 'tex_pal_efb_copy_retlut', 'tex_pal_efb_copy_rewrite']
EFB_PAL = ['tex_pal_efb_copy', 'tex_pal_efb_copy_retlut', 'tex_pal_efb_copy_rewrite']
# the indirect/bump scenes (scenes_indirect.hpp) sample an RGBA8 texture created with GX_CLAMP, and the JPA scenes an RGBA8 one
IND = ['tev_ind_addprev', 'tev_ind_bias_st', 'tev_ind_coord_scale', 'tev_ind_exp_neg', 'tev_ind_itf3', 'tev_ind_itf4', 'tev_ind_itf5',
       'tev_ind_matrix', 'tev_ind_warp', 'tev_ind_wrap']
JPA = ['tp_jpa_line', 'tp_jpa_point']
SWAP_RB = '(([](uint32_t p) { return (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu); })(@))'

controls = [
    # ---- CMPR ----
    dict(name='CMPR: four-colour test inverted (c0 < c1)', file=CONV, old='  table[1] = p1;\n  if (c0 > c1) {',
         new='  table[1] = p1;\n  if (c0 < c1) {', fails=['tex_cmpr']),
    dict(name='CMPR: sub-blocks 1 and 2 swapped (TR/BL order)', file=CONV,
         old='    cmpr_subblock(s + 8, d + 4, stride);\n    cmpr_subblock(s + 16, d + 4 * stride, stride);',
         new='    cmpr_subblock(s + 16, d + 4, stride);\n    cmpr_subblock(s + 8, d + 4 * stride, stride);', fails=['tex_cmpr']),
    dict(name='CMPR: index bit order reversed within a row', file=CONV,
         old='    d[0] = table[(bits >> 6) & 3];\n    d[1] = table[(bits >> 4) & 3];\n    d[2] = table[(bits >> 2) & 3];\n    d[3] = table[bits & 3];',
         new='    d[0] = table[bits & 3];\n    d[1] = table[(bits >> 2) & 3];\n    d[2] = table[(bits >> 4) & 3];\n    d[3] = table[(bits >> 6) & 3];', fails=['tex_cmpr']),
    dict(name='CMPR: interpolation 4/8 + 4/8 instead of 3/8 + 5/8', file=CONV,
         old='r |= (((3 * ((a >> shift) & 0xff) + 5 * ((b >> shift) & 0xff)) >> 3) & 0xff) << shift;',
         new='r |= (((4 * ((a >> shift) & 0xff) + 4 * ((b >> shift) & 0xff)) >> 3) & 0xff) << shift;', fails=['tex_cmpr']),
    dict(name='CMPR: 3-colour mode index 3 opaque instead of transparent', file=CONV,
         old='table[3] = table[2] & 0x00ffffffu;', new='table[3] = table[2];', fails=['tex_cmpr']),
    # ---- plain formats ----
    dict(name='I4: nibbles swapped', file=CONV,
         old='        d[2 * k] = kI4[s[k] >> 4];\n        d[2 * k + 1] = kI4[s[k] & 0xf];',
         new='        d[2 * k] = kI4[s[k] & 0xf];\n        d[2 * k + 1] = kI4[s[k] >> 4];', fails=['tex_fmt_i4']),
    dict(name='I4: alpha forced opaque (should equal the intensity)', file=CONV,
         old='t[n] = pack(ex4(n), ex4(n), ex4(n), ex4(n));', new='t[n] = pack(ex4(n), ex4(n), ex4(n), 0xff);', fails=['tex_fmt_i4']),
    dict(name='I4: 4 -> 8 bit expansion n*16 instead of n*17', file=CONV,
         old='t[n] = pack(ex4(n), ex4(n), ex4(n), ex4(n));', new='t[n] = pack(n << 4, n << 4, n << 4, n << 4);', fails=['tex_fmt_i4']),
    dict(name='I8: alpha forced opaque (should equal the intensity)', file=CONV,
         old='d[x] = s[x] * 0x01010101u;', new='d[x] = (s[x] * 0x00010101u) | 0xff000000u;', fails=['tex_fmt_i8']),
    dict(name='I8: texels reversed within a tile row', file=CONV,
         old='d[x] = s[x] * 0x01010101u;', new='d[x] = s[7 - x] * 0x01010101u;', fails=['tex_fmt_i8']),
    dict(name='IA4: alpha and intensity nibbles swapped', file=CONV,
         old='t[b] = pack(ex4(b & 0xf), ex4(b & 0xf), ex4(b & 0xf), ex4(b >> 4));',
         new='t[b] = pack(ex4(b >> 4), ex4(b >> 4), ex4(b >> 4), ex4(b & 0xf));', fails=['tex_fmt_ia4']),
    dict(name='IA8 (plain texture only): alpha and intensity bytes swapped', file=CONV,
         old='d[x] = ia8(s + 2 * x);', new='d[x] = pack(s[2 * x], s[2 * x], s[2 * x], s[2 * x + 1]);', fails=['tex_fmt_ia8']),
    dict(name='RGB565 (plain texture only): red and blue swapped', file=CONV,
         old='d[x] = rgb565(s + 2 * x);', new='d[x] = ' + SWAP_RB.replace('@', 'rgb565(s + 2 * x)') + ';', fails=['tex_fmt_rgb565']),
    dict(name='RGB565 shared decoder: red and blue swapped (plain, TLUT entries, CMPR endpoints)', file=CONV,
         old='inline uint32_t rgb565(const uint8_t* s) noexcept { return k565Hi[s[0]] | k565Lo[s[1]]; }',
         new='inline uint32_t rgb565(const uint8_t* s) noexcept { return ' + SWAP_RB.replace('@', 'k565Hi[s[0]] | k565Lo[s[1]]') + '; }',
         fails=['tex_fmt_rgb565', 'tex_pal_c8_rgb565', 'tex_cmpr']),
    dict(name='RGB5A3 (plain texture only): alpha forced opaque', file=CONV,
         old='d[x] = rgb5a3(s + 2 * x);', new='d[x] = rgb5a3(s + 2 * x) | 0xff000000u;', fails=['tex_fmt_rgb5a3']),
    dict(name='RGB5A3 shared decoder: alpha forced opaque (plain + every RGB5A3 TLUT)', file=CONV,
         old='inline uint32_t rgb5a3(const uint8_t* s) noexcept { return k5a3Hi[s[0]] | k5a3Lo[s[0] >> 7][s[1]]; }',
         new='inline uint32_t rgb5a3(const uint8_t* s) noexcept { return (k5a3Hi[s[0]] | k5a3Lo[s[0] >> 7][s[1]]) | 0xff000000u; }',
         fails=['tex_fmt_rgb5a3'] + RGB5A3_TLUT),
    dict(name='RGBA8: red and blue swapped', file=CONV,
         old='d[x] = pack(s[2 * x + 1], gb[2 * x], gb[2 * x + 1], s[2 * x]);',
         new='d[x] = pack(gb[2 * x + 1], gb[2 * x], s[2 * x + 1], s[2 * x]);',
         fails=['texture_replace', 'texture_modulate', 'line_tex_offset_off', 'line_tex_offset_on', 'tex_wrap', 'tex_mip',
                'tex_mip_off', 'tex_mip_truncated'] + IND + JPA),
    # ---- guest-memory palettes ----
    dict(name='TLUT IA8: alpha and intensity bytes swapped', file=CONV,
         old='out[i] = ia8(src + 2 * i);', new='out[i] = pack(src[2 * i], src[2 * i], src[2 * i], src[2 * i + 1]);',
         fails=['tex_pal_c4_ia8']),
    dict(name='TLUT RGB565 entries decoded as RGB5A3', file=CONV,
         old='out[i] = rgb565(src + 2 * i);', new='out[i] = rgb5a3(src + 2 * i);', fails=['tex_pal_c8_rgb565']),
    dict(name='TLUT RGB5A3 entries decoded as RGB565', file=CONV,
         old='out[i] = rgb5a3(src + 2 * i);', new='out[i] = rgb565(src + 2 * i);', fails=RGB5A3_TLUT),
    dict(name='C4: nibbles swapped', file=CONV,
         old='        d[2 * k] = pal[s[k] >> 4];\n        d[2 * k + 1] = pal[s[k] & 0xf];',
         new='        d[2 * k] = pal[s[k] & 0xf];\n        d[2 * k + 1] = pal[s[k] >> 4];', fails=['tex_pal_c4_ia8']),
    dict(name='C8: texels reversed within a tile row', file=CONV,
         old='d[x] = pal[s[x]];', new='d[x] = pal[s[7 - x]];',
         fails=['tex_pal_c8_rgb5a3', 'tex_pal_c8_rgb565', 'tex_pal_two_tluts']),
    # ---- wrap, mips ----
    dict(name='wrap: MIRROR mapped to REPEAT', file=TEX, old='case GX_MIRROR:\n    return GL_MIRRORED_REPEAT;',
         new='case GX_MIRROR:\n    return GL_REPEAT;', fails=['tex_wrap']),
    dict(name='wrap: REPEAT mapped to CLAMP', file=TEX, old='case GX_REPEAT:\n    return GL_REPEAT;',
         new='case GX_REPEAT:\n    return GL_CLAMP_TO_EDGE;', fails=['tex_wrap']),
    dict(name='wrap: CLAMP mapped to REPEAT', file=TEX, old='  case GX_CLAMP:\n  default:\n    return GL_CLAMP_TO_EDGE;',
         new='  case GX_CLAMP:\n  default:\n    return GL_REPEAT;', fails=['tex_wrap'] + IND),
    dict(name='mips: NEAR_MIP_NEAR mapped to a plain NEAREST filter', file=TEX,
         old='case GX_NEAR_MIP_NEAR:\n    return mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;',
         new='case GX_NEAR_MIP_NEAR:\n    return GL_NEAREST;', fails=['tex_mip', 'tex_mip_truncated']),
    dict(name='mips: a mip filter is used even when the GL texture has one level (incomplete texture)', file=TEX,
         old='const bool mips = ref.mipCount > 1;', new='const bool mips = true;', fails=['tex_mip_off']),
    dict(name='mips: a truncated chain is not completed down to 1x1', file=TEX,
         old='return decoded > 1 ? full_mip_count(w, h) : 1;', new='return decoded;', fails=['tex_mip_truncated']),
    # ---- palette caches (guest memory) ----
    dict(name='static palette object cache ignores the TLUT object id', file=TEXGX,
         old='entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == tlut.tlutObjId &&\n          entry.tlutDataVersion == tlut.tlutDataVersion) {',
         new='entry.texDataVersion == obj.texDataVersion &&\n          entry.tlutDataVersion == tlut.tlutDataVersion) {', fails=['tex_pal_two_tluts']),
    dict(name='static palette object cache ignores the TLUT data version (re-pointed object)', file=TEXGX,
         old='entry.tlutObjId == tlut.tlutObjId &&\n          entry.tlutDataVersion == tlut.tlutDataVersion) {',
         new='entry.tlutObjId == tlut.tlutObjId) {', fails=['tex_pal_two_tluts']),
    dict(name='content cache key ignores the TLUT hash', edits=[
        (TEXGX, 'tlutHash.low64 == rhs.tlutHash.low64 && tlutHash.high64 == rhs.tlutHash.high64 &&\n           tlutFormat == rhs.tlutFormat',
         'tlutFormat == rhs.tlutFormat'),
        (TEXGX, 'key.mipCount, key.tlutHash.low64, key.tlutHash.high64, key.tlutFormat, key.tlutEntries);',
         'key.mipCount, key.tlutFormat, key.tlutEntries);')], fails=['tex_pal_two_tluts']),
    # ---- palette caches / GPU palette pass (EFB copies) ----
    dict(name='TLUT texture served stale after the TLUT object is re-pointed', file=TEXGX,
         old='if (cache.tlutTexture.handle && cache.tlutTexture.tlutDataVersion == tlut.tlutDataVersion) {',
         new='if (cache.tlutTexture.handle) {', fails=['tex_pal_efb_copy_retlut']),
    dict(name='derived palette textures kept across a TLUT re-point (in-place update hits the earlier draw)', file=TEXGX,
         old='    cache.dynamicPaletteTextures.clear();\n    for (const u32 texObjId : cache.staticTextureUsers) {',
         new='    for (const u32 texObjId : cache.staticTextureUsers) {', fails=['tex_pal_efb_copy_retlut']),
    dict(name='replay never refreshes derived textures before a draw', file=REPLAY,
         old='  refresh_derived_textures(*program, draw);\n', new='', fails=EFB_PAL),
    dict(name='replay never marks an EFB copy as rewritten', file=REPLAY,
         old='      mark_texture_written(*dst); // textures derived from this copy (palette lookups) must be recomputed\n', new='',
         fails=['tex_pal_efb_copy_rewrite']),
    # ---- memory safety / teardown ----
    dict(name='unit-15 revert: texture caches sized for the 8 GX maps only (ASan)', asan=True, file=REPLAY,
         old='constexpr uint32_t kTextureUnits = kFogRangeUnit + 1;', new='constexpr uint32_t kTextureUnits = GX_MAX_TEXMAP;',
         fails=[], crash=r'AddressSanitizer: [a-z-]*overflow(.|\n)*bind_texture'),
    dict(name='teardown: shutdown_gl() no longer releases the texture caches', file=REPLAY,
         old='  gx::shutdown();\n  shutdown_textures();\n', new='', fails=['teardown']),
    dict(name='pending-delete list a plain static: a texture held by a static is destroyed after it (use-after-destroy at exit, ASan)',
         asan=True, edits=[
        (TEX, 'static auto* mutex = new std::mutex;\n  return *mutex;', 'static std::mutex mutex;\n  return mutex;'),
        (TEX, 'static auto* pending = new std::vector<PendingDelete>;\n  return *pending;',
         'static std::vector<PendingDelete> pending;\n  return pending;')],
         fails=[], crash=r'AddressSanitizer: heap-use-after-free(.|\n)*~TextureRef'),
]

if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
