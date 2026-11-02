import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

SH = ROOT + '/vita/aurora/overlay/gx/vita_shader.cpp'
VX = ROOT + '/vita/aurora/overlay/gx/vita_vertex.cpp'
FR = ROOT + '/vita/aurora/overlay/gfx/vita_frame.cpp'
MAIN = ROOT + '/tests/host/e2e/e2e_main.cpp'

POINTS = ['point_size', 'tp_point_2d', 'tp_map_path_point', 'tp_jpa_point']
FIRST_ENDPOINT = '''        std::memcpy(d, src, stride);
        for (uint32_t k = 0; k < layout.count; ++k) {
          const DecodedAttr& ea = layout.attrs[k];
          if (ea.attr != GX_VA_NULL && ea.attr != GX_VA_POS && ea.attr != GX_VA_PNMTXIDX) {
            std::memcpy(d + ea.offset, raw.data() + static_cast<size_t>(a) * stride + ea.offset,
                        ea.kind == DecodedAttr::Kind::Float ? ea.components * 4u : 4u);
          }
        }
        if (cornerOff >= 0) {'''

controls = [
    dict(name='point size doubled', file=SH,
         old='float point_size = line_width * min(viewport_scale.x, viewport_scale.y);',
         new='float point_size = 2.0 * line_width * min(viewport_scale.x, viewport_scale.y);', fails=POINTS),
    dict(name='line width doubled', file=SH,
         old='float lw = line_width * min(viewport_scale.x, viewport_scale.y);',
         new='float lw = 2.0 * line_width * min(viewport_scale.x, viewport_scale.y);',
         # offscreen_map_render (added later) draws a 12/6 = 2 logical pixel line into a 128x96 target and checks its thickness
         fails=['line_width', 'line_tex_offset_on', 'tp_line_ext', 'tp_line_j2d', 'tp_map_path_line', 'tp_jpa_line',
                'offscreen_map_render']),
    dict(name='TEV A0 alpha read as 1', file=SH, old='case GX_CA_A0:\n    return "tevreg0.a";',
         new='case GX_CA_A0:\n    return "1.0";', fails=['tp_line_ext']),
    dict(name='register material colour read as 0', file=SH,
         old='matSrc = at >= 0 ? fmt::format("uVS[{}]", at) : "vec4(1.0)"s;',
         new='matSrc = at >= 0 ? "vec4(0.0)"s : "vec4(1.0)"s;',
         fails=['tp_point_2d', 'lighting_diffuse', 'lighting_masks']),
    dict(name='KCSEL_1_4 (= 2/8) read as 3/8', file=SH, old='case GX_TEV_KCSEL_2_8:\n      return "vec3(2.0 / 8.0)";',
         new='case GX_TEV_KCSEL_2_8:\n      return "vec3(3.0 / 8.0)";', fails=['tp_map_path_point']),
    dict(name='line strip drops its last segment', file=VX, old='segments = vtxCount >= 2 ? vtxCount - 1 : 0;',
         new='segments = vtxCount >= 3 ? vtxCount - 2 : 0;', fails=['tp_line_j2d', 'tp_map_path_line']),
    dict(name='points: last vertex dropped', file=VX, old='  } else {\n    segments = vtxCount;\n  }',
         new='  } else {\n    segments = vtxCount > 0 ? vtxCount - 1 : 0;\n  }', fails=POINTS),
    dict(name='line quads take every non-position attribute from the first endpoint', file=VX,
         old='        std::memcpy(d, src, stride);\n        if (cornerOff >= 0) {', new=FIRST_ENDPOINT,
         fails=['tp_line_j2d', 'tp_jpa_line']),
    dict(name='indexed array stride + 4', file=VX, old='p.arrayStride = m.stride;', new='p.arrayStride = m.stride + 4;',
         fails=['display_list_indexed', 'tp_map_path_line', 'tp_map_path_point']),
    dict(name='array snapshots kept across frames (begin_frame fix removed)', file=FR,
         old='  for (auto& array : gx::g_gxState.arrays) {\n    array.cachedRange = {};\n  }\n  gx::g_gxState.dirty |= gx::DirtyImmediates;\n',
         new='', fails=['tp_map_path_point']),
    dict(name='attribute list: points also get aPosOther / aPnMtxIdxOther', file=VX,
         old='    if (config.lineMode != 3) {\n      add(GX_VA_NULL, Kind::Float, 3, "aPosOther");',
         new='    if (true) {\n      add(GX_VA_NULL, Kind::Float, 3, "aPosOther");',
         fails=['tp_point_2d', 'tp_map_path_point', 'tp_jpa_point']),
    dict(name='vertex shader declares every attribute of the list, used or not', file=SH,
         old='    const auto& a = m_layout.attrs[i];\n    if (std::find(m_usedAttrs.begin(), m_usedAttrs.end(), a.name) == m_usedAttrs.end()) {\n      continue;\n    }\n',
         new='    const auto& a = m_layout.attrs[i];\n', fails=['tp_map_path_line']),
    dict(name='harness cap lowered from 16 to 5 attributes (the cap clause itself)', file=MAIN,
         old='constexpr uint32_t kVitaAttribs = 16;', new='constexpr uint32_t kVitaAttribs = 5;',
         fails=['tp_line_j2d', 'tp_map_path_line', 'tp_jpa_line']),
]

if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
