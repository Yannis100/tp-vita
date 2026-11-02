# TP-on-Vita: route to a playable port (revised 2026-10-22 — Dusklight + Aurora GX route)

This file records the concrete route from the current state (the whole Dusklight
game code compiling for `arm-vita-eabi`) to a playable Twilight Princess on Vita.

> **Starting a new session? Read [`HANDOVER.md`](HANDOVER.md) first.** It records the
> current status, the verified facts, the traps, and the open problems in one place.

**Route decision (supersedes the earlier plan):** we build on the **Dusklight**
game code (TwilitRealm, CC0, runs on PC today) rather than the raw zeldaret/tp
decomp with a hand-written GX layer. Dusklight's render path is **Aurora**
(encounter/aurora): a GX compatibility layer whose heart is the **GX command
processor** (`extern/aurora/lib/gx/command_processor.cpp`) that walks the game's
pre-recorded GX command lists and drives a backend. On desktop that backend is
**WebGPU/Chromium Dawn** (D3D12/Vulkan/Metal, WGSL), which has no Vita target.

The Vita port therefore keeps Dusklight's game code *and* Aurora's GX command
processor, and **replaces only the backend**: Aurora's `webgpu/*`, `gfx/*` and
SDL/nod backends are swapped for Vita-native ones, ultimately feeding **vitaGL
(GXM)** — the ACGC-style GX→GL path the skill documents as proven.

The GX hardware calls from the game land on Aurora's `lib/gx/*` command
processor through the `GX*` / `GD*` / `VI*` / `DVD*` surfaces. TP models ship
with pre-recorded GX command lists, so a real command processor is required; the
same process Aurora uses on desktop (FD → FEF state keys, TEV → shader codegen)
is where the workload lives.

## Current state (2026-10-22)

- ✅ JSystem (`tpv_jsystem`) and the whole game code (`tpv_game`: `m_Do`,
  `f_*`, `d_*`, actors, Z2Audio, SSystem, DUSK pc-layer) **compile** for
  `arm-vita-eabi` with the shared PCH — **EXIT=0, zero errors**.
  - 1225 game objects + 242 JSystem objects; 8 static libs
    (`tpv_aurora`, `tpv_borealis`, imgui, rmlui, miniz, xxhash, nod_lite, …).
- ✅ **Fixes made this session:**
  - `vita/include/webgpu/webgpu.h` — Vita shim for the C header the mods
    service header includes (`WGPUBlendState` order, typed
    `WGPUColorTargetState`, corrected initializer macro).
  - `_C` macro collision: VitaSDK newlib `ctype.h` defines `_C` = `040`;
    `d/d_event_lib.h` undefs it before its `u16 _C` member.
  - Excluded Desktop/WebGPU-bound sources from `tpv_game` and stubbed them
    Vita-side:
    - `dusk/mods/svc/gfx.cpp`, `dusk/mods/svc/camera.cpp` (mods service,
      WebGPU-typed),
    - `dusk/ui/icon_provider.cpp` (RmlUI icon provider, pulls the wgpu C++
      layer) → `vita/src/dusk_ui_icon_provider_stub.cpp`.
- ✅ **Executable** (`tpv_vita` ELF links, no unresolved symbols):
  - `vita/src/main.cpp` — Vita `main` (clocks, heap/stack settings) forwarding to Aurora's
    `aurora_main` (the game's `main` is renamed by `<aurora/main.h>`).
  - `vita/aurora/backend.cpp` — Aurora C API on vitaGL (`aurora_initialize` with vitaGL init,
    `aurora_update` input pump, `aurora_begin_frame/end_frame`, `aurora_shutdown`, `aurora_get_*`),
    reports `BACKEND_OPENGLES`; `vita/aurora/imgui_vita.cpp` is the ImGui renderer. The GX API itself
    comes from the merged Aurora tree (see M2/M3), not from stubs.
  - `vita/aurora/aurora_stubs.cpp` — no-op window, RmlUi and texture-replacement entry points,
    `g_config`/`g_gameName` (`aurora_imgui_add_texture` now lives in `imgui_vita.cpp`).
  - `vita/src/dusk_mods_stub.cpp` — unavailable gfx/camera mod service modules;
    `vita/src/libc_stubs.cpp` — `execv` and `dl*` (VitaSDK libdl needs taiHEN).
  - Real Aurora `gx/dl.cpp` (display-list parser) and `gx/attr_fmt.cpp` are built from
    patched copies (cmake/aurora.cmake swaps the WebGPU-typed `gx.hpp` include for a
    two-declaration header) — the M3 vertex decoder builds on these.
  - The old hand-guessed `system_stubs.cpp`/`gx_stubs.cpp` never compiled and are
    parked in `vita/aurora/_hand_written_unbuilt/` for reference only.
  - SELF/VPK wiring is opt-in (`-DTPV_PACKAGE=ON`); the VPK builds and passes `check_vpk.py`
    (7 entries, 0 errors, 0 warnings).

## Milestones

### M1. Everything compiles for ARM — DONE
- JSystem + all game code build for `arm-vita-eabi` (see Current state).

### M2. Real GX frontend linked, VPK builds — DONE
- `tpv_vita` ELF links, SELF/VPK build with `-DTPV_PACKAGE=ON` (`check_vpk.py`: 7 entries,
  0 errors, 0 warnings).
- The no-op GX stubs are gone: the real Aurora GX frontend (GX API, FIFO, BP/XF register
  decode, command processor, display-list parser) is compiled from a merged tree
  (`build/aurora_lib` = upstream `lib/` + `vita/aurora/overlay/` + `vita/aurora/patches.py`,
  assembled by `tools/build_aurora_tree.py`). The draw replay and texture modules that were
  placeholders at this milestone are implemented in M3 below.

### M3. vitaGL renderer for the Aurora frontend — CODE WRITTEN, builds+links; partly host-tested
**Verification status (checked 2026-10-26):**
| Piece | Evidence |
|---|---|
| Vertex decode (`vita_vertex.cpp`) | host tests, 71,063 checks vs an independent reference, ASan/UBSan (`tests/host/vertex/run.sh`) |
| Shader generator (`vita_shader.cpp`) | host test (`tests/host/shader/run.sh`, sanitizers via `TPV_CXXFLAGS`/`TPV_LDFLAGS`): 7,000 random valid configs (seed 7, ASan/UBSan) all compile+link as GLSL ES 1.00 on Mesa, incl. ~12% line/point configs since 2026-10-26; 0 segment-table errors; u32-lane flag contract holds on every config. Pixel output only for the e2e scenes. Not tested: vitaGL's Cg-based translator. Line/point configs add vertex attributes: the fuzz reaches 19 (vitaGL cap 16) in 9 of 7,000 configs, but those are not TP configs. **Measured against what TP draws (2026-10-27, `scenes_tp_lines.hpp`, 7 scenes built from the game's real line/point draws):** the largest decodes 6 attributes (declares 6), far under 16; behaviour above 16 on vitaGL stays unverified, and nothing TP draws needs it |
| Texture decoders (`vita_texture_convert.cpp`) | host differential test: byte-identical to upstream Aurora's converter for all colour + palette formats, 3,780 checks (`tests/host/texture/run.sh`). Not tested: GL upload, samplers, caches |
| GL replay, EFB/present | host end-to-end harness (`tests/host/e2e`, 2026-10-26/27): **87 synthetic GX scenes** (+ a teardown check) through the real command processor -> `vita::replay` -> EFB -> present on Mesa, **14,242 pixel checks**, plain and AddressSanitizer both clean, **133 scripted negative controls** (`tests/host/e2e/controls/`; each runs the whole suite and must fail exactly the scenes written next to it): core state (blend, depth, matrices, lighting and per-draw light masks, scissor/viewport, EFB copy re-use), ortho + perspective fog and the fog-range table, texture formats through GL (CMPR, I/IA, RGB565/5A3, wrap, mips, C4/C8 palettes, palette after EFB copy), indirect/bump TEV, TP's real line/point draws, alpha write masks and dst-alpha replace, offscreen targets (`GXCreateFrameBuffer`), present scaling to 960x544 and other sizes. It found and fixed: u32 light masks read as float denormals (every light skipped); a texture-unit-15 cache overflow (ASan); the palette pass never wired (`prepare_texture_for_sampling` never called); `shutdown_textures()`/`gx::shutdown()` never called (ASan use-after-free at exit); per-frame array offsets (`cachedRange`) kept across frames. **Not live in this build, so no scene:** the dst-alpha clear quad (`JFWDisplay::clearEfb` is `STUB_RET()` under `TARGET_PC`, its only caller of `GXSetDstAlpha`) and `GXSetZTexture` (nothing decodes it); offscreen targets are live (shadows, map, bloom, blur). Not covered or hardware-only: max-LOD clamp (not implemented), 1/128-texel indirect rounding, alpha bump / dynamic indirect matrices / emboss, RGB5A3 quantisation and 6-bit EFB alpha, Z8/Z16 copies (unsupported), `gl_FragCoord.z` precision, unit-15 sampling, NPOT wrap/mips, oversized or negative-origin `glViewport` (the bloom pass), >16 attributes, vitaGL's Cg translator and limits |
| First-boot screens, boot log, shader dump | host tests (`tests/host/disc/run.sh`: 14 + 10,686 checks, ASan/UBSan; 23 scripted controls in `disc_controls.py`; `check_printf_formats.sh`), **and one headless Vita3K run (2026-10-27, v0.2.1 build 4236, firmware 3.74, `tests/vita3k/`)**: the VPK installs, the eboot loads, three screens render (no disc / short-copy / wrong game + RVZ), `boot.log` is written and rotated, O/X exits with code 0. The run found that the VitaSDK's newlib has no `%z` (log line printed `zu`); fixed. Vita3K is an emulator: no game frame, no vitaGL, no `libshacccg.suprx`, no real controller; the game log location is still unverified because the game never started |
| Backend, ImGui renderer | **never executed anywhere** (compile + link only) |
| Uniform caps (Vita: 128 vertex / 16 fragment vec4) | vertex side mitigated 2026-10-26: a draw whose matrix index is provably 0 (no `GX_VA_PNMTXIDX` in the format **and** `GXSetCurrentMtx` value 0, `pn_mtx_provably_zero`) uploads one pn/nrm matrix bank, moving unskinned lit + dynamic texture matrix from 138 → 84 vertex vec4 (`run.sh canon` pin I). The first version trimmed on "no `GX_VA_PNMTXIDX`" alone, which is wrong: `aPnMtxIdx` is always a decoded attribute holding the draw's current matrix and the game uses `GXSetCurrentMtx(3)` (= PNMTX1, `d_home_button.cpp`), so those draws would have read past the uploaded bank. The program cache is now keyed on the trim; pin I2 (current matrix 1) keeps full banks (138, residual overflow). Fragment side stays a warning guard; random heavy configs still exceed (fs max 30, vs max 152). **Light slots (2026-10-26):** the upstream blob carries 8 lights (40 vec4); the generator now carries only as many `Light` slots as a draw enables. The replay reads the draw's light-mask lanes (`light_mask_location` + `lights_in_use`), picks a bucket of 1/2/4/6/8 slots, and keys the program cache on it (3 bits; emboss texgens raise the count to cover the lights they name). Skinned + lit + dynamic texture matrix: 138 (8 slots) -> 108 / 118 / 128 vec4 at 2 / 4 / 6 slots, all inside the cap (`run.sh canon` pins J2/J4/J6; fuzz contracts for the segment table, the u32 lanes, the slot count and the mask lookup). **Residual:** 7 or 8 enabled lights on that material still reach 138, and how often Twilight Princess does that is unknown (masks come from the disc's model data) |
| Fog range table | implemented 2026-10-26, **corrected the same day**: shader samples `uFogRange` (unit 15, 1-row RGBA8) at `gl_FragCoord.x * uFogRangeSize.x` and multiplies `fogBase`; replay `bind_fog_range()` re-encodes the table from `frame.storage` each time it differs. The first version clamped the factors (always >= 1) to [0,1] in 8 bits, so every texel was exactly 1.0 (a silent no-op), sampled at `+0.5` past the pixel centre (texel boundary), skipped `fogRangeBase == 0`, and cached by (base, width) although offsets repeat each frame. Now 16-bit fixed point over [0, 32] (`fog_range_encode`), host-tested on Mesa incl. a GPU decode round trip (`tests/host/shader/run.sh fog`) with negative controls. TP enables it (`d_kankyo.cpp`). The sampling on vitaGL (`gl_FragCoord`, unit 15, NEAREST) is hardware-unverified |
| Depth EFB copies (Z8/Z16) | **not supported** (white fill, as before). A CPU readback was tried and removed: `glReadPixels(GL_DEPTH_COMPONENT)` is invalid in GLES2 and in vitaGL (colour formats only, `GL_INVALID_ENUM`), so it left an all-zero depth texture (= "everything at the near plane", worse than white). Route: `vglTexImageDepthBuffer(GL_TEXTURE_2D)` + conversion shader; needs vitaGL built with `STORE_DEPTH_STENCIL=1` (unverified for the vdpm build), hardware-only |
| FBO row order (`kFboRowsFlipped`) | assumption, unverified (depends on how the vdpm vitaGL was built) |

Architecture (kept from upstream, backend swapped):
```
game -> GX API -> FIFO (Drain mode, same thread) -> command_processor -> gfx::vita::Frame
                                                      (raw BE vertex bytes, u16 indices, packed
                                                       uniform blobs, array snapshots, op list)
aurora_end_frame -> gfx::finish() -> vita::replay(Frame) -> GLES2 (vitaGL) -> EFB FBO -> present
```
Pieces (contract: `vita/aurora/overlay/gx/vita_gx.hpp`; GL include point: `gfx/vita_gl.hpp`):
| Piece | File(s) | Status |
|---|---|---|
| Frame recorder (bump buffers, op list, pipeline/bind-group registries) | `gfx/vita_frame.{hpp,cpp}` | done |
| Vertex layout + CPU decode (GX BE formats -> interleaved floats/ubytes), line/point expansion | `gx/vita_vertex.cpp` | done (compiles+links) |
| TEV/lighting/fog/texgen -> GLSL ES 1.00 + stage uniform maps (+ uniform-budget guard: 16 frag / 128 vert vec4; fog-range LUT sampler; current-matrix-0 pn-matrix bank trim; per-draw light-slot buckets) | `gx/vita_shader.cpp` | done (compiles+links) |
| GX texture decode (all formats, TLUT, CMPR, mips) + GL texture objects + caches | `gx/texture.cpp`, `gfx/vita_texture.cpp` | done (compiles+links) |
| GL replay: state (blend/depth/cull/scissor/viewport), program cache, EFB FBO, EFB copies (colour only; Z8/Z16 unsupported), fog-range upload, present | `gfx/vita_gl_replay.cpp` | done (compiles+links) |
| Real `aurora_initialize` (vitaGL init, OS/VI/FIFO/input init), `BACKEND_OPENGLES` | `vita/aurora/backend.cpp` | done (compiles+links) |
Decisions:
- **Runtime GLSL** through vitaGL's translator (needs the user's `libshacccg.suprx` in `ur0:data/`),
  with the GX state key (`ShaderConfig`) as program-cache key; no precompiled shader bank (TP's
  TEV state space is not enumerable offline the way ACGC's is).
- **Depth**: `UseReversedZ=false` (patch); the vertex shader maps Aurora's `z' = z + w` to GL clip z.
- **Render size**: EFB is an FBO at the game's configured VI size (native GX resolution,
  e.g. 640x480 / widescreen-widened), presented to 960x544 with aspect fit.
- **Threading**: all GL on the thread that called `vglInit` (the game thread); FIFO runs in `Drain` mode.
- **Host verification**: the GL code is plain GLES2, so the shader/decoder sources build for x86
  against Mesa (`tests/host`). `tests/host/e2e` renders synthetic GX scenes through the real command processor ->
  frame -> replay -> EFB -> present and checks pixels; it is the only way to exercise the replay without a Vita, and
  it cannot model vitaGL's own shader translator or limits (see its README).

### M4. System HAL — input/audio DONE (code level), rest pending hardware
- **Input: DONE (code).** `aurora::input::initialize()` in `aurora::initialize`; `aurora_update()`
  pumps SDL events and handles GAMEPAD_ADDED/REMOVED (controller hotplug). SDL3 Vita backend maps
  the physical pad to a virtual controller; on-hardware mapping check still pending.
- **Audio: DONE (code).** `dusk::audio::Initialize()` runs via `JAUInitializer` (TARGET_PC) →
  software DSP (DuskDsp) + SDL3 stream → SceAudio. DspStub CRASH paths replaced with safe no-ops
  (dead code; the real DSP is DuskDsp). On-hardware audio open still pending.
- `nod_lite` (already present) reads a plain GameCube ISO/CISO; untested (no disc image here).
  **First boot: handled in code (probe logic host-tested; injection never run on a Vita).** Nothing else tells the game where
  the disc is: with no `--dvd` and no saved `backend.isoPath` it enters `launchUILoop()`, which waits for
  the RmlUi pre-launch UI (stubbed here) and would loop on a blank screen. `vita/src/main.cpp` therefore
  searches `ux0:data/tpvita/`, `ux0:data/tpvita/rom/` and `ux0:data/tpvitaskel/rom/` (`vita/src/disc_probe.hpp`)
  for a plain `.iso`/`.gcm` or `.ciso` whose game ID is GZ2E01/GZ2J01/GZ2P01 (what nod_lite can read and the
  game's catalog accepts; `GZ2E01.*` first) and injects `--dvd <path>`. The README documents the location.
  **Diagnosis (code done 2026-10-26; the screens are host-tested as pixels, never shown on a display):** `main()` now writes
  `ux0:data/tpvita/boot.log` (firmware, free memory, argv, whether `libshacccg.suprx` exists, every disc-like file
  with its game ID, the game's own `dusk::iso::inspect` verdict) and runs the pure decision `tpv::decide_disc`
  (found disc -> saved `backend.isoPath` from config.json -> error). If no disc is usable it draws a text screen
  straight to the display (`boot_screen.cpp`: CDRAM block + `sceDisplaySetFrameBuf`, 5x8 font drawn for this
  project) saying why (no disc / wrong game / Wii disc / RVZ etc. / a plain image shorter than 1,459,978,240 bytes)
  and waits for O or X. It does not stop for a missing `libshacccg.suprx`: Vita3K may not need it, so that is a log
  warning. The game's own log is flushed from Info up (`borealis_cli_stub.cpp`), so a crash loses little; it lives in
  `ux0:/data/TwilitRealm/Dusklight/logs/` (SDL3's Vita pref path, read from `libSDL3.a` strings, not run). The GL replay
  logs `compiling gx-XXXXXXXX ...` before each program, `ready in N ms` after, `frame N` for the first five frames
  and every 300th, and the GL strings; a program the translator refuses is saved to
  `ux0:data/tpvita/shader_fail/gx-XXXXXXXX.{vert,frag}.glsl` (at most 8 pairs).
  **Still open:** none of this has run on a Vita. In Vita3K (headless, 2026-10-27) the CDRAM framebuffer recipe displays the
  screens and O/X exits; on hardware it is untested, and so is the location of the game log (the game never started).
- `pc_os/vi`: VI framebuffer ↔ vitaGL surface; OS time/threads.
- `pc_card`: save files under `ux0:data/tpvita/` (the path `backend.cpp` uses).

### M5. Bringup, perf, packaging — BRINGUP REACHED (2026-10-29), perf and hardware pending
- ✅ **Boot and main loop reached in Vita3K**: `mDoMch_Create()` returns 1, `main01()` enters
  `do { … } while (dusk::IsRunning)` and iterates; real frames rendered with a content bbox of
  `(271,128)–(1008,671)` on the 1280×800 emulator canvas (the pixel-exact prediction of the
  608×448 EFB plus `present()`'s aspect fit). Zero failing allocations on a clean run.
- Still open: title-screen/gameplay milestone beyond the main loop, reference comparison of the
  rendered output, real-hardware run, and all performance work (no FPS measurement exists).
- `ATTRIBUTE2=12` (365 MiB main) already set in CMake; heap/stack retuned by measurement
  (see `HANDOVER.md` §4.4 — root reserve 192 KiB, game heap 23 MiB); the game asks
  `aurora_initialize` for `mem1Size=256 MiB`.
- **Feasibility on real hardware is no longer an open question.** A separate port
  ([`NathanKneT/DuskOnVita`](https://github.com/NathanKneT/DuskOnVita)) runs this same game code
  on a PS Vita 1000 at **12-28 FPS**, with working saves, input, touch and gyro, and a native GXM
  renderer as its default backend. Its first build did 6.3 FPS on 22 Oct. Its base commit is 3
  commits behind ours on the same Aurora pin, so the route is confirmed. This does not validate
  *our* port, but it removes the risk that the approach cannot work. See `HANDOVER.md` §7.5.

## Key facts

| Item | Value |
|---|---|
| Game code | Dusklight (CC0), branch `vita` (**14 uncommitted edits**, incl. endian.h ARM LE, mods/api.h 64-bit-only checks, d_event_lib.h `_C` macro, main.h no process restart, audio/DspStub.cpp CRASH -> no-op, heap sizes + root reserve + the `!TARGET_VITA` guard on the retail arena trim in m_Do_machine.cpp, JKRExpHeap `dataPtr` ordering, and the J3DJointTree null-deref fix) |
| Backend today | Aurora WebGPU/Dawn (desktop) → **vitaGL/GXM** (Vita) |
| GX entry points the game calls | ~325 across `GX*/GD*` (per undefined-symbol scan), many via J3D `J3DGDWrite*` |
| Proven reference | ACGC (GameCube C decomp → Vita GX-HLE on vitaGL, 52 precompiled TEV shaders, playable) |
| Memory | `ATTRIBUTE2=12` → 365 MiB main; game wants 256 MiB MEM1 + 24 MiB MEM2 |

## Honesty

- Everything compiles, links and packages. **Nothing has run on a Vita.** In Vita3K (headless) the VPK installs,
  and with a user-supplied disc image in `ux0:data/tpvita/` the game **boots, reaches its main loop and renders
  frames** (bbox `(271,128)–(1008,671)`; after the 2026-11-02 texture-upload fix, 78,360–87,870 distinct
  colours per screenshot, up from ~1,600–4,000 while every texture upload was silently failing). Nobody has confirmed the
  rendered scene is the *correct* scene — there is no reference comparison against a real GameCube or the desktop
  build. Audio is disabled, and card saves, the pause menu and most content are untested. One unexplained
  intermittent crash remains (`HANDOVER.md` §7.1, ~1 run in 7); its size arithmetic is now sane
  (the retail arena trim that silently removed 24 MiB per run is guarded, arena 40 → 64 MiB) but the
  failing 3.0 GiB allocation itself is still unexplained and must not be described as solved. The GL
  replay has also run on the host against Mesa (synthetic scenes only, no game content, no vitaGL).
  See the M3 verification table.
- Two claims were **downgraded** after the DuskOnVita audit (`HANDOVER.md` §7.5): the J3DJointTree
  null dereference is real and fixed but *latent*, since that port ships byte-identical code and
  boots, so it is not established as our crash cause; and feasibility is no longer an open question,
  because that port runs on hardware at 12-28 FPS.
- **Not releasable as a binary yet.** vitaGL is LGPL-3.0-or-later and we link the prebuilt VitaSDK
  archive rather than a named source revision, so the relink right is not demonstrable. `NOTICE`
  records the two ways to fix it. Until then this is a source-only project.
- The Aurora `gfx/*` + `webgpu/*` layer is deeply wgpu-typed and is **not**
  carried to Vita; we implement the narrow surface the game + command processor
  actually need (state + draw + texture + TEV).
- Final FPS is unverified on hardware; multi-month effort for full playability.
