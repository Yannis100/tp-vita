# Host end-to-end render harness

Renders small synthetic GX scenes through the **real** pipeline on an x86 host and checks the pixels:

```
GX API calls -> FIFO (Drain) -> command processor -> gfx::vita::Frame -> gfx::finish()
             -> vita::replay() (GLES2) -> EFB FBO -> present() -> glReadPixels
```

The GL calls go to Mesa (llvmpipe) through a headless EGL surfaceless GLES2 context (`tests/host/glctx.*`, shared with the shader test).
Everything above the GL calls is the same code that the Vita build compiles.

```sh
tests/host/e2e/run.sh                  # all scenes (builds the merged Aurora tree + harness first)
tests/host/e2e/run.sh lighting_diffuse # one or more scenes by name
TPV_E2E_ASAN=1 tests/host/e2e/run.sh   # the same under AddressSanitizer (own object directory)
TPV_E2E_VERBOSE=1 tests/host/e2e/run.sh clear   # also print info-level log lines
TPV_E2E_PNG=/some/dir ...              # where per-scene PNGs go (default /tmp/tpv/e2e/png)
```

Needs: `g++`, Mesa with EGL + GLES2 (`libEGL.so.1`, `libGLESv2.so.2`), `zlib`, python3, the VitaSDK (`$VITASDK`, only for
the GL/fmt headers), the fetched deps (`tools/fetch_deps.sh`) and the Dusklight checkout (`AURORA_DIR`, default
`../dusklight/extern/aurora`). No X11 headers are needed (the build defines `EGL_NO_X11`; `libx11-dev` is not a dependency). Build output goes to `/tmp/tpv/e2e` (`OBJ`, `OUT`; `TPV_E2E_OUT` for the binary run by `run.sh`).

## Scenes: 87, plus a teardown check

| Group (file) | Scenes | What they pin |
|---|---|---|
| core (`e2e_main.cpp`) | 26 | clear, orientation, vertex colours, blend, depth, matrices, texture replace/modulate, Lambert lighting and per-draw light masks, alpha test, culling, display-list indexed arrays, line width / point size and their texcoord offsets, scissor, viewport, EFB copy re-use, orthographic fog and the game's fog-range table |
| perspective fog (`e2e_main.cpp`) | 5 | `GX_FOG_PERSP_LIN` with TP's real parameters, the same fog written by `GFSetFog` into a display list, ground plane to the horizon; EXP/EXP2/REVEXP (not a TP path) |
| textures (`scenes_texture.hpp`) | 18 | CMPR, I4/I8/IA4/IA8/RGB565/RGB5A3, wrap modes, mipmaps (on / off / truncated chain), C4/C8 palettes with two TLUTs and the palette-after-EFB-copy cases |
| indirect / bump TEV (`scenes_indirect.hpp`) | 10 | warp, matrix, exponent, coordinate scale, bias, wrap, add-previous, ITF formats 3/4/5 |
| TP line/point draws (`scenes_tp_lines.hpp`) | 7 | the draws TP really issues: ext line, 2D point, J2D line, map path line/point, JPA line/point; includes an audit of the vertex-attribute count per draw (see below) |
| alpha and dst-alpha (`scenes_alpha.hpp`) | 11 | colour/alpha write masks, the shadow pack (`d_drawlist.cpp` 1547), copy-with-clear and frame clear obeying the update bits, dst-alpha replace and its blend factors on RGBA6 and RGB8 EFBs, RGB8 copies opaque, alpha blend equation, depth-only draws |
| offscreen targets (`scenes_offscreen.hpp`) | 6 | `GXCreateFrameBuffer`/`GXRestoreFrameBuffer`: clear, own depth, re-use per size, viewport/scissor restore, logical vs render viewport (`d_map_path.cpp`), the bloom layout (`m_Do_graphic.cpp`), the blur pyramid |
| present scaling (`scenes_present.hpp`) | 4 | EFB 640x480 into 960x544 (the Vita screen), 640x600, 700x500, 320x544: size, rounding, margins, black clear |

Every scene carries its derivation next to the expectation. Some scenes need more than the default: a `warmup` frame run first (the clear
colour is captured at `begin_frame`; the letterbox scene fills the window white first), or a present size (`presentW`/`presentH`, default 960x600).

## What a pass proves

- The command processor, shader generator, uniform packing, vertex decode, texture upload, state replay and
  EFB/present agree with each other and with analytic expectations (barycentric colours, texel centres, TEV maths,
  Lambert lighting, per-draw light masks, depth order, blending, culling, alpha test, line/point expansion, indexed big-endian arrays,
  fog, indirect warps, palettes, alpha/dst-alpha, offscreen passes, present scaling).
- Orientation is pinned (`orientation` scene): GX row 0 is the top of the image through EFB and present.
- Line/point draws: the largest TP draw decodes 6 attributes (declares 6), against vitaGL's cap of 16; the fuzzed worst case is 19.
  The audit in `scenes_tp_lines.hpp` prints both numbers per draw.
- Each scene has a negative control that fails it (below). A scene that passes on the first try is not trusted until it has been made to fail.

## What a pass does not prove

- **Mesa is not vitaGL.** vitaGL translates GLSL ES 1.00 to Cg/GXP with its own compiler (needs `libshacccg.suprx`),
  has smaller uniform limits (128 vertex / 16 fragment vec4), no `glBlendColor`, `glBlendFunci`, `glDrawBuffers`,
  `glLogicOp`, and no depth read-back. This harness does not model any of that.
- Hardware-only: FBO row order (`kFboRowsFlipped`), `gl_FragCoord` semantics and `gl_FragCoord.z` precision, texture unit 15 under vitaGL,
  NPOT wrap and mips, oversized or negative-origin `glViewport` (the bloom pass uses one), more than 16 vertex attributes,
  the Cg translator. Performance, memory use and the SGX543's precision are not modelled either.
- A fresh GL texture is zero-filled on Mesa, so "a new offscreen target is cleared" can only be proven on **re-use** (`offscreen_reuse_sizes`).
- A copy rectangle larger than its source clamps to the source's edge texels; a too-small target can hide behind uniform edges. The offscreen
  scenes give the last columns and rows their own colours for that reason.
- Not live in this build, so no scene: the destination-alpha clear quad (`JFWDisplay::clearEfb`, the only caller of `GXSetDstAlpha`) and
  `GXSetZTexture(GX_ZT_REPLACE)` are `STUB_RET()` under `TARGET_PC`; nothing in the renderer decodes `GXSetZTexture`. Offscreen targets **are** live
  (`GXCreateFrameBuffer` at `d_drawlist.cpp:1593`, `d_map_path.cpp:477`, `m_Do_graphic.cpp:943/1567/1759`).
- Gaps: GX max-LOD clamp is not implemented; the indirect scenes do not pin 1/128-texel rounding; alpha bump, dynamic indirect matrices and
  emboss texgens are not covered; RGB5A3 copies pass through at 8 bits (no quantisation) and EFB alpha is 8 bits, not 6; depth EFB copies
  (Z8/Z16) are unsupported (white); anti-aliased pixel formats are not exercised.
- Differences too small to see: a half-pixel shift in the fog-range LUT sampling changes the factor by far less than
  the test tolerance. The fog-range reference is a transcription of upstream's formula, so it checks the Vita
  path (encode, upload, sampling, multiply), not upstream's maths.
- Anything on a real disc image.

## Negative controls

A control is valid only if the change reaches the build and fails the scenes it is meant to fail and no others. `controls/e2e_ctl.py`
runs each control against the **whole** suite and compares the failing set exactly; a control that fails nothing did not reach the build,
one that fails extra scenes is not specific. Files are restored and compared byte for byte, and a final clean run must pass. The runner checks
every definition before it runs anything (unknown key, wrong tuple length, missing file, duplicate name): a 3-item `mutate_copy` once crashed the fog
group at its last control because the runner unpacks 5 items. All six groups were run from these committed scripts on 2026-10-27 (133 controls,
0 problems); a control added before the later scenes needed its expected set widened when those scenes appeared, so re-run a group after adding scenes.

```sh
export VITASDK=...                                              # as for run.sh
python3 tests/host/e2e/controls/alpha_controls.py               # one group
python3 tests/host/e2e/controls/alpha_controls.py "replay: offscreen target cleared to alpha 1"   # one control by name
```

| Script | Controls | Covers |
|---|---|---|
| `fog_controls.py` | 4 | perspective formula, `B - z`, 16-bit depth quantisation, `GFSetFog` writing C from the wrong plane (mutated copy of the game file) |
| `texture_controls.py` | 38 | CMPR/format decode, wrap, mip selection, palette upload/lookup/re-TLUT, EFB-copy palettes |
| `indirect_controls.py` | 31 | every indirect-stage field: matrix, exponent, bias, wrap, scale, add-previous, ITF |
| `tp_lines_controls.py` | 13 | line/point expansion, width, attribute-count handling |
| `alpha_controls.py` | 27 | update masks, `efb_has_alpha`, copy/frame clear, dst-alpha shader/uniform/blend/two-pass split, `DSTALPHA` mapping, blend equation, offscreen clear and filter |
| `offscreen_present_controls.py` | 20 | begin/end, nested begin, viewport/scissor restore, per-size cache key, swapped dimensions, depth, copy source and offsets, logical vs render viewport/scissor, line width scaling, present scale/rounding/margins/clear |

133 controls in all; each was run, and each fails exactly the set written next to it. The first-boot code has its own 23 in
`tests/host/disc/disc_controls.py`.

Control keys: `file`/`old`/`new` or `edits` (overlay sources; `old` must occur exactly once), `upstream=(relpath, old, new)` for an upstream
Aurora file with no overlay copy (the runner writes a mutated copy into the overlay for the run and deletes it), `mutate_copy` for a game file,
`asan` (run under AddressSanitizer), `crash` (a regex that a non-clean run must print). See the docstring of `e2e_ctl.py`.
Do not run `tests/host/shader/run.sh canon` or edit overlay files while a control run is in progress: the merged tree is shared.

Earlier manual controls (not scripted; each was applied by hand and reverted):

| Break this | Scenes that must fail |
|---|---|
| comment out `convert_int_lanes(...)` in `gfx/vita_gl_replay.cpp` `upload_uniforms` | `lighting_diffuse`, `line_tex_offset_on` |
| `pn_mtx_provably_zero`: return `attrType == GX_NONE` only | `current_matrix` |
| `glScissor` in `apply_scissor`: use the whole target | `scissor` |
| `apply_viewport`: `glViewport` over the whole target | `viewport` |
| `kFboRowsFlipped = true` (the hardware-risk assumption) | `orientation`, `texture_replace`, `efb_copy_reuse` |
| fog offset `fogBase - c` -> `fogBase + c` in the fragment generator | `fog_ortho_linear` |
| `fog_range_encode`: clamp factors to [0,1] (the original no-op) | `fog_range_on` (`fog_range_off` stays green) |
| replay: always pass 1 light slot to `get_program` (instead of `lights_in_use(...)`) | `lighting_masks` (all four quads) |

## Conventions worth copying

- Capture the clear colour at `begin_frame` like upstream does: the first frame after `GXInit` is black. Render a
  warm-up frame first.
- Scenes share one GX state. `base_state()` resets everything a scene may change (line width, point size, tex offsets, pixel format: the EFB
  has no alpha in `RGB8_Z24`, GX's default; the alpha scenes opt in with `rgba6()` as `JFWDisplay::preGX` does).
- GX does not re-emit an unchanged viewport or scissor, so restoring both after an offscreen pass is mandatory; `base_state()` cannot heal a
  stale one.
- Keep an expectation's derivation next to it. GX front faces are visually clockwise here (upstream builds its
  pipelines with `wgpu::FrontFace::CW`); lights built with `GXInitLightDir` need a position (an infinite light is a far
  `GXInitLightPos`).
- When a scene fails, check the scene, then the harness, then the code. Of the first failures here, one was a scene bug (a light that
  was never positioned), one a harness assumption (row order), and one a real renderer bug. A control that fails the "wrong" scenes is
  usually telling you something true (a cached target leaking between scenes), so analyse it before adjusting the expectation.
- Fuzz only inside the valid domain. Run the whole harness under ASan now and then.

## Bugs this harness found

- **Light masks** and the point/line texcoord mask are `u32` bit fields in the uniform blob. The Vita shader read them as
  floats, where a small integer is a denormal (= 0), so every light was skipped and every lit surface rendered ambient-only; line/point
  texture offsets never applied. The shader generator tests (link-only) did not see it. Fixed with `UniformSeg::intLanes` and
  `convert_int_lanes` at upload; the shader host test now checks the contract on every fuzzed config.
- **Texture unit 15 overflow** (found by running the harness under ASan): `bind_fog_range` binds the fog-range table on unit 15, but the replay's
  per-unit state cache (`sCache`) was too small for it, so `bind_texture` wrote past the array into neighbouring globals.
- **The palette pass was never wired** (found by the palette scenes): `prepare_texture_for_sampling` was never called, so the GPU palette pass
  was dead and EFB-copy palette textures sampled empty data.
- **`shutdown_textures()` / `gx::shutdown()` were never called** (ASan heap-use-after-free at exit: `~TextureRef` pushed into an already-destroyed
  pending-delete list). `shutdown_gl()` now runs both and the pending list is deliberately leaked; the `teardown` pseudo-scene checks that live
  texture bytes return to zero. Static-destruction order of GL-handle caches matters.
- **Stale per-frame storage offsets**: the array cache kept `cachedRange` across frames, so a draw in frame N+1 used a snapshot offset from
  frame N (`tp_map_path_point`). `begin_frame()` now clears it and marks the immediates dirty.
- **Found outside this harness, by running the VPK in Vita3K:** the boot log read `zu disc-like file(s)` because the VitaSDK's newlib has no `%z`
  length modifier (glibc, which every host test uses, accepts it). `tests/host/check_printf_formats.sh` now guards that (`%ll` does work).
