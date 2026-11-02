import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_ctl import run_controls, ROOT, DUSKLIGHT, TMP

SHADER = ROOT + '/vita/aurora/overlay/gx/vita_shader.cpp'
GFPIXEL = DUSKLIGHT + '/libs/dolphin/src/gf/GFPixel.cpp'
PERSP = ['fog_persp_linear', 'fog_persp_gf_displaylist', 'fog_persp_far', 'fog_persp_ground', 'fog_persp_types']
LINE = 'float fogBase = uFS[{}].x / (uFS[{}].y - gl_FragCoord.z);'

controls = [
    dict(name='perspective branch uses the orthographic formula (A * z)', file=SHADER, old=LINE,
         new='float fogBase = uFS[{}].x * gl_FragCoord.z + 0.0 * uFS[{}].y;', fails=PERSP),
    dict(name='perspective fog: B - z becomes B + z', file=SHADER, old=LINE,
         new='float fogBase = uFS[{}].x / (uFS[{}].y + gl_FragCoord.z);', fails=PERSP),
    dict(name='depth quantised to 16 bits before the fog divide (precision sensitivity)', file=SHADER, old=LINE,
         new='float fogBase = uFS[{}].x / (uFS[{}].y - floor((gl_FragCoord.z + 0.0 * uFS[0].y) * 65535.0 + 0.5) / 65535.0);',
         fails=['fog_persp_far']),
    dict(name='GFSetFog writes C from nearz instead of startz (the game file, via a mutated copy)',
         mutate_copy=(GFPIXEL, 'GFPIXEL_SRC', os.path.join(TMP, 'GFPixel_mut.cpp'), 'C = startz / (endz - startz);',
                      'C = nearz / (endz - startz);'),
         fails=['fog_persp_gf_displaylist']),
]
if __name__ == '__main__':
    only = sys.argv[1:] or None
    sys.exit(1 if run_controls(controls, only) else 0)
