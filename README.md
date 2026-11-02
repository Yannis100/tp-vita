# Zelda: Twilight Princess — PS Vita port (Dusklight + Aurora GX route)

A native recompile of the **Twilight Princess** game code for the **PS Vita**
(`arm-vita-eabi`), built on [TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight).
The game code and JSystem are compiled for ARM, Aurora's GX command processor drives a new
**vitaGL (GLES2)** renderer, and the build produces an installable VPK (see [`PLAN.md`](PLAN.md)).

**Current status: builds, links and packages (`build/tpv_vita.vpk` passes `check_vpk.py`), and the
game boots and renders.** In Vita3K (headless, `tests/vita3k/`) with a user-supplied disc image in
`ux0:data/tpvita/`, `mDoMch_Create()` returns 1, `main01()` reaches its main loop and keeps
iterating, and real frames are rendered with a content bbox of `(271,128)–(1008,671)` on a 1280×800
canvas, which is the pixel-exact result predicted by the 608×448 EFB and the aspect-fit in
`present()`. Zero failing allocations and no crash markers on a clean run. Screenshots carry
78,360–87,870 distinct colours; before the texture-upload fix of 2026-11-02 they carried only
~1,600–4,000, because every GX texture upload was silently failing (see `HANDOVER.md` §7.3).

**What that does not prove.** Vita3K is an emulator: nothing here has run on real hardware. The
screenshots show a rendered 3D scene, but nobody has confirmed it is the *correct* scene (no
reference comparison against a real GameCube or the desktop build). Audio is disabled
(`DUSK_AUDIO_DISABLED`); card saves, the pause menu and most of the game's content have never been
exercised. Treat "boots and renders" as the accurate claim, not "playable".

The vertex decoder, shader generator, texture decoders and disc discovery are tested on the host (`tests/host/`),
and synthetic GX scenes (not game content) are rendered end to end through the real command processor ->
GL replay -> EFB -> present path on Mesa (`tests/host/e2e`, 87 scenes, 14,242 pixel checks, 133 scripted negative
controls). Mesa is not vitaGL, so vitaGL's shader translator and the Vita GPU are still unverified; the backend,
input and audio have only been compiled. No game assets are included — you supply your own disc image.

## What "compiles" means today

- `tpv_jsystem`: JSystem (`J2D*`, `J3D*`, `JUT*`, `JAS*`, …) — 242 objects.
- `tpv_game`: `m_Do`/`f_*`/`d_*`/actors/Z2Audio/SSystem + Dusklight's PC layer —
  1225 objects, sharing the game PCH.
- `tpv_aurora`: Aurora's target-neutral system layer (OS, mtx, GD, SI, VI, THP,
  card, DVD via `nod_lite`) + `tpv_borealis` + imgui/rmlui/miniz/xxhash deps.
- Shared flags: `-fsigned-char -fno-short-enums` (Vita ARM defaults to 1-byte
  enums; the game's data layouts assume int-sized enums), `ATTRIBUTE2=12`
  (365 MiB main).

## Route (why not Dusklight/Aurora as shipped)

Dusklight renders through **Aurora**, whose GX command processor
(`extern/aurora/lib/gx/command_processor.cpp`) is exactly what we want — but its
backend is **WebGPU/Chromium Dawn** (D3D12/Vulkan/Metal, WGSL), which has no Vita
target. The Vita port keeps the game code *and* Aurora's GX command processor,
and replaces only the backend with a **vitaGL/GXM** implementation (the ACGC
pattern: GameCube GX → GL on Vita, TEV → precompiled CG shaders). See
[`PLAN.md`](PLAN.md) for the full milestone plan.

## Layout

```
tp-vita/
├─ CMakeLists.txt              # Vita build (VITASDK required, TP_DEPS=dusklight)
├─ cmake/                      # aurora.cmake, borealis.cmake, deps.cmake,
│                              # game_sources.cmake (JSystem capture)
├─ tools/fetch_deps.sh         # fetch deps/ (abseil, imgui, rmlui, …)
├─ patches/abseil-vita.patch   # abseil fixups for arm-vita-eabi
├─ vita/
│  ├─ include/                 # Vita shadow headers (webgpu/webgpu.h shim, …)
│  ├─ aurora/                  # Vita Aurora backend: backend.cpp (C API on vitaGL),
│  │                           #   imgui_vita.cpp, aurora_stubs.cpp (window/RmlUi/texture
│  │                           #   replacement stubs), overlay/ (GX/gfx files that replace
│  │                           #   upstream's WebGPU ones), patches.py
│  ├─ src/                     # main.cpp (Vita entry + first-boot checks), disc_probe.hpp (finds the disc image),
│  │                           #   boot_diag.hpp / boot_text.hpp / boot_font.hpp (error screens, boot log; host-tested),
│  │                           #   boot_screen.cpp (puts them on the display), text_pad.cpp (link-layout filler),
│  │                           #   *_stub.cpp (borealis/dusk), libc_stubs.cpp (execv, dl*)
│  └─ nod_lite/                # C disc reader (replaces Rust nod)
├─ tests/host/                 # x86 tests: vertex, shader, texture, disc, e2e render harness (+ Mesa GLES2 context)
├─ tests/vita3k/               # run the VPK in Vita3K headless (podman); first-boot screens + boot.log
├─ sce_sys/                    # LiveArea assets (icon0, bg, startup)
└─ legacy/m0-selftest/         # earlier Tier-1 logic skeleton (moved here)
```

## Requirements

- **VitaSDK** (`arm-vita-eabi-gcc`, `vita-create-*`, vitaGL, `vitashark`, `taihen`, SDL3), `cmake`, `python3`.
- A checkout of **TwilitRealm/dusklight** (branch `vita`) at `../dusklight`
  (or point `TP_DEPS` at it).
- `tools/fetch_deps.sh` run once (fetches into `deps/`).

## Build

```sh
tools/fetch_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTPV_PACKAGE=ON
cmake --build build -j
# optional sanity check; the script ships with the vita-port skill, so substitute your own path
python3 ~/.roo/skills/vita-port/scripts/check_vpk.py build/tpv_vita.vpk
```

`build/aurora_lib` (upstream Aurora `lib/` + `vita/aurora/overlay/*` + `vita/aurora/patches.py`) is
assembled at configure time; edits to the overlay re-run the assembly automatically. The link step
runs `tools/check_text_gap.py`; if it reports too small a gap behind the text segment, change
`-DTPV_TEXT_PAD_KIB=<n>` (see `vita/src/text_pad.cpp`).

Host tests (x86, Mesa llvmpipe for the GL ones): `tests/host/{vertex,shader,texture,disc,e2e}/run.sh` (shader: also `run.sh canon` and `run.sh fog`; e2e: `TPV_E2E_ASAN=1` for AddressSanitizer, controls in `tests/host/e2e/controls/`, `tests/host/disc/disc_controls.py`; `tests/host/check_printf_formats.sh`; Vita3K: [`tests/vita3k/README.md`](tests/vita3k/README.md); e2e: see [`tests/host/e2e/README.md`](tests/host/e2e/README.md) for what it proves and what it cannot).

## Installing and running (for players)

You supply the game and one Sony module; neither is in the VPK.

1. Install `tpv_vita.vpk` (VitaShell). Needs HENkaku/enso and the **`libshacccg.suprx`** shader
   compiler in `ur0:data/` (the same file other vitaGL ports need).
2. Copy your own **GameCube** Twilight Princess image (USA `GZ2E01`, Japan `GZ2J01` or PAL `GZ2P01`) as a
   plain **`.iso` / `.gcm`** or **`.ciso`** file to `ux0:data/tpvita/` (or `ux0:data/tpvita/rom/`). Any file
   name works; `GZ2E01.iso` is tried first. RVZ/WIA/GCZ/WBFS/NFS images and the Wii release are not readable.
3. Start the app. It finds the image, checks it with the game's own disc check, and boots it (`--dvd` is
   injected). The first start creates `ux0:data/tpvita/` if it is missing.
   If there is no usable image, the app shows a dark-blue screen with a red title that says why
   ("no disc image found", or "the disc image cannot be used" with the file, its size and the reason; a
   copy that was cut short is recognised by its size). It lists the folders it searched and what it found in
   them. Press O or X to leave.

### If something goes wrong

| What you see | Look here |
|---|---|
| A red-titled message screen | The message itself: it names the file and the problem. |
| Black screen, crash, or back to LiveArea | `ux0:data/tpvita/boot.log` (what the app saw and decided before the game started; the previous run is `boot.log.1`). Its last line tells how far start-up got. Then the game's own log, `ux0:/data/TwilitRealm/Dusklight/logs/dusklight-*.log`. |
| The game log ends with `compiling gx-XXXXXXXX ...` | vitaGL's shader translator died on that program. |
| `ux0:data/tpvita/shader_fail/gx-XXXXXXXX.*.glsl` exists | The translator refused that shader; the files are the exact source it was given, and the log has its error message. At most 8 pairs are written. |
| `boot.log` says `libshacccg.suprx ... not found` | On a real Vita, put the module in `ur0:data/`. Vita3K may not need it. |

The game log is flushed at every information line, so a crash loses at most the debug lines since the last
one. `frame N: ... draws, ... programs compiled so far` appears for the first five frames and then every
300th, so the last such line shows how far rendering got.

*(unverified: none of this has run on hardware. The game-log folder is read from the SDL3 library's strings
and the borealis source; the screens are checked on the host as pixels, not on a display.)*

## Honesty notes

- **Build-verified**: game code + JSystem + the Vita backend compile for `arm-vita-eabi`, the ELF links
  with no unresolved symbols, and the VPK packages and passes `check_vpk.py` (7 entries,
  0 errors, 0 warnings).
- **Host-verified**: vertex decode (71k checks vs an independent reference), shader generation (random valid
  GX configs, incl. lines/points, link as GLSL ES 1.00 on Mesa), texture decoding (byte-identical to
  upstream Aurora), disc discovery, the first-boot diagnosis (font, text layout, disc scan, the disc decision table, config lookup,
  boot log, shader-source dump: 10,686 checks, 23 negative controls), and 26 synthetic scenes rendered through command processor -> replay -> EFB ->
  present on Mesa (triangle colours, depth, blending, culling, alpha test, textures, lighting, lines/points,
  indexed display lists, scissor, viewport, EFB copy re-use, fog, fog range with the game's own table, per-draw light masks). That harness found a real bug: light masks were uploaded as raw `u32` and read as
  floats, so every light was skipped (fixed 2026-10-26).
- **Never executed**: dst-alpha replace, perspective fog types, indirect/bump TEV, texture formats beyond RGBA8
  through GL, offscreen targets, present scaling to 960x544, backend, ImGui renderer, input, audio, anything under real vitaGL (its shader translator, the
  `kFboRowsFlipped=false` and `gl_FragCoord` assumptions), performance, runtime memory, and `nod_lite` against a
  real disc image.
- **Build-verified only** (first-boot diagnosis): the on-screen error screen (CDRAM framebuffer + `sceDisplaySetFrameBuf`),
  the gate in `main()` that runs the game's `dusk::iso::inspect` on the disc, the game-log flush level, the
  log breadcrumbs, and the hook that saves a refused shader's source.
- **Known gaps**: skinned, lit materials with dynamic texture matrices are within the 128-vec4 vertex-uniform cap
  for up to 6 simultaneously enabled lights (the generator carries only as many light slots as a draw enables);
  7 or 8 enabled lights on such a material still reach 138. How often Twilight Princess does that is unknown
  (the light masks come from the disc's model data);
  depth EFB copies (Z8/Z16) are unsupported (filled white). Fog range is implemented but only host-tested.
- Matrix/vector/quaternion math is the upstream portable C (Aurora's
  `lib/dolphin/mtx/*`), not byte-for-byte PPC cycle-matched.

## Legal

Ports only open-source code. No commercial game data is included; users must
supply their own dumped copy of Twilight Princess. Dusklight is CC0-1.0;
Aurora and Borealis are MIT; the Vita layer in this repo is CC0-1.0.

[`NOTICE`](NOTICE) records every component, the exact revision or archive it is
built from, and its licence; the licence texts are in [`LICENSES/`](LICENSES/).
Nothing there is redistributed: the Dusklight checkout is a submodule built in
place, and the dependency archives are fetched at configure time and gitignored.

**One caveat before publishing a binary:** vitaGL is LGPL-3.0-or-later, and this
port currently links the prebuilt archive shipped in VitaSDK rather than a source
revision named in this repository. That makes the LGPL relink right hard to
demonstrate. `NOTICE` spells out the two ways to fix it. Until one is done, treat
this repository as source-only.
