import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

RP = ROOT + '/vita/aurora/overlay/gfx/vita_gl_replay.cpp'
SH = ROOT + '/vita/aurora/overlay/gx/vita_shader.cpp'

OB, OD, OR, OM, BL, PY = ('offscreen_basic', 'offscreen_depth_isolation', 'offscreen_reuse_sizes', 'offscreen_map_render',
                          'tp_bloom_layout', 'offscreen_pyramid')
TSP = 'tp_shadow_pack'
ALL_OFF = [TSP, OB, OD, OR, OM, BL, PY]
PV, PL, PR, PD = ('present_vita_960x544', 'present_letterbox_640x600', 'present_round_700x500', 'present_downscale_320x544')

controls = [
    # ---- offscreen begin / end ----
    # (a stale viewport or scissor also fails offscreen_pyramid: it reuses the cached 64x64 target)
    dict(name='offscreen: a nested begin overwrites the suspended EFB', file=RP,
         old='      if (suspended == nullptr) {\n        suspended = sCurrent;\n      }\n      Target* target = offscreen_target',
         new='      suspended = sCurrent;\n      Target* target = offscreen_target', fails=[TSP, OR]),
    dict(name='offscreen: end does not return to the EFB', file=RP,
         old='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        bind_target(*suspended);\n        suspended = nullptr;',
         new='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        suspended = nullptr;', fails=ALL_OFF),
    dict(name='offscreen: end rebinds the EFB framebuffer but not its viewport / scissor', file=RP,
         old='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        bind_target(*suspended);\n        suspended = nullptr;',
         new='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        sCurrent = suspended;\n        glBindFramebuffer(GL_FRAMEBUFFER, suspended->fbo);\n        suspended = nullptr;',
         fails=[OB, OD, PY, BL, TSP]),
    dict(name='offscreen: begin keeps the target\'s old viewport', file=RP,
         old='      target->viewport = {0.f, 0.f, static_cast<float>(op.width), static_cast<float>(op.height), 0.f, 1.f};\n',
         new='', fails=[OR, PY]),
    dict(name='offscreen: begin keeps the target\'s old scissor', file=RP,
         old='      target->scissor = {0, 0, static_cast<int32_t>(op.width), static_cast<int32_t>(op.height)};\n',
         new='', fails=[OR, PY]),
    dict(name='offscreen: targets keyed by width only', file=RP, old='(uint64_t{width} << 32) | height;',
         new='(uint64_t{width} << 32);', fails=[OR]),
    dict(name='offscreen: target created with width and height swapped', file=RP,
         old='create_target(*target, width, height);', new='create_target(*target, height, width);', fails=[OB, OR, OM, BL]),
    dict(name='offscreen: depth cleared to 0 at begin', file=RP,
         old='clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 0.f}, 1.f, true);',
         new='clear_current(true, true, true, Vec4<float>{0.f, 0.f, 0.f, 0.f}, 0.f, true);', fails=[OD]),
    dict(name='offscreen: end clears the EFB depth', file=RP,
         old='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        bind_target(*suspended);\n        suspended = nullptr;',
         new='    case Op::Kind::EndOffscreen:\n      if (suspended != nullptr) {\n        bind_target(*suspended);\n        glDepthMask(GL_TRUE);\n        glClearDepthf(1.f);\n        glClear(GL_DEPTH_BUFFER_BIT);\n        sCache.passValid = false;\n        suspended = nullptr;',
         fails=[OD]),
    # ---- copies from a target ----
    dict(name='copy: source is always the EFB', file=RP, old='Target& source = *sCurrent;', new='Target& source = sEfb;', fails=ALL_OFF),
    dict(name='copy: source rectangle y offset ignored', file=RP, old='float v0 = static_cast<float>(r.rect.y) * invH;',
         new='float v0 = 0.f;', fails=[BL]),
    dict(name='copy: source rectangle x offset ignored', file=RP,
         old='draw_quad(q, static_cast<float>(r.rect.x) * invW,', new='draw_quad(q, 0.f * invW,', fails=[BL]),
    # ---- logical vs render viewport / scissor ----
    dict(name='GXSetViewportRender: width halved', upstream=('gx/command_processor.cpp',
         '    set_render_viewport({\n        .left = left,\n        .top = top,\n        .width = width,',
         '    set_render_viewport({\n        .left = left,\n        .top = top,\n        .width = width / 2,'), fails=[OM, BL]),
    dict(name='GXSetScissorRender: width and height halved', upstream=('gx/command_processor.cpp',
         'set_render_scissor({left, top, width, height});', 'set_render_scissor({left, top, width / 2, height / 2});'),
         fails=[OM]),
    dict(name='shader: line width / point size not scaled by render / logical viewport', file=SH,
         old='vec2 viewport_scale = rvp / max(lvp, vec2(1.0));', new='vec2 viewport_scale = vec2(1.0);', fails=[OM]),
    # ---- present ----
    dict(name='present: scale = max(w/e, h/f)', file=RP, old='const double scale = std::min(', new='const double scale = std::max(',
         fails=[PV, PL, PR, PD]),
    dict(name='present: width truncated instead of rounded', file=RP,
         old='const auto width = static_cast<GLint>(std::lround(sEfb.width * scale));',
         new='const auto width = static_cast<GLint>(sEfb.width * scale);', fails=[PR]),
    dict(name='present: x margin = (window - width) / 3', file=RP,
         old='glViewport((static_cast<GLint>(sPresentWidth) - width) / 2,', new='glViewport((static_cast<GLint>(sPresentWidth) - width) / 3,',
         fails=[PV, PR]),
    dict(name='present: y margin = (window - height) / 3', file=RP,
         old='(static_cast<GLint>(sPresentHeight) - height) / 2, width,', new='(static_cast<GLint>(sPresentHeight) - height) / 3, width,',
         fails=[PL, PD]),
    dict(name='present: no black clear before the blit', file=RP,
         old='  glClear(GL_COLOR_BUFFER_BIT);\n\n  const double scale', new='\n  const double scale', fails=[PV, PL, PR, PD]),
]

if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
