# Handover: Twilight Princess on PS Vita

**Written:** 2026-10-29. **Updated:** 2026-11-11.
**For:** a fresh session picking up this port. Read this first, then `PLAN.md` and `README.md`.

This document records state, verified facts, traps and open work. Guesses are labelled as guesses.
Measurements include their numbers so the next session does not repeat them.

Section numbers are stable: `PLAN.md`, `README.md` and the host tests cite §4.4, §7.1, §7.3, §7.4b
and §7.5 by number. Do not renumber.

---

## 1. What this project is

The Legend of Zelda: Twilight Princess (GameCube) ported to the PS Vita, packaged as a VPK.

It is **not** a from-scratch reimplementation. The stack:

| Layer | Source | Notes |
|---|---|---|
| Game code | `dusklight` (decompilation) | checkout in `dusklight/`, pinned to `77705f8bb95ef7b7dca4675b149c4bcb6a48da34` |
| Renderer frontend | `aurora` | nested at `dusklight/extern/aurora`, pinned by dusklight to `pre-upgrade-3227d76` |
| GLES2 translation | `vitaGL` | `r1488.g2bdbe89` from VitaSDK (§7.4c) |
| Toolchain | VitaSDK `arm-vita-eabi` GCC 15.2.0, SDL3 3.4 | `VITASDK=/home/yanserm1/vitasdk` is a symlink; the tree is at `/home/yanserm1/psvita/tooling/vitasdk` |
| Vita glue | `tp-vita/` | **the only code this port owns** |

`tp-vita/` is a git repo (`Yannis100/tp-vita`, public). `build/` and `deps/` are gitignored, so a
commit does not protect the tree you build from. `dusklight/` is a separate repo and carries the port's
game-side fixes as **uncommitted changes**; they are not in the `tp-vita` remote.

### The merged-tree architecture (important)

`build/aurora_lib` is generated at configure time by `tools/build_aurora_tree.py` from three inputs:

1. `dusklight/extern/aurora/lib/**` — upstream, unmodified
2. `vita/aurora/overlay/**` — **whole-file replacements** (same relative path wins)
3. `vita/aurora/patches.py` — **exact-once string substitutions**, applied after the overlay

Consequences:

- Never edit `build/aurora_lib` **or upstream `extern/aurora/lib`** directly. Both are ineffective:
  the tree is regenerated and your edit is dropped. Only `overlay/` and `patches.py` reach the build.
- Each `patches.py` entry must match **exactly once**. A missing anchor fails the build loudly.
- Overlay files are full replacements, so an upstream change must be re-reviewed by hand. There is no merge.

---

## 2. Current status: honestly

**The game boots, reaches its main loop, renders, and on a driven run reaches real gameplay.**

- Build is clean: `cmake --build` exits 0 and produces `build/tpv_vita-release.vpk`; text→data gap
  20,144 bytes (checked by `tools/check_text_gap.py`).
- `mDoMch_Create()` returns 1. `main01()` stays in its `do { … } while (dusk::IsRunning)` loop.
- Frames are real: 78,360–87,870 distinct colours each (before the §7.3 fix: 1,614–3,979, i.e.
  untextured), content bbox `(271,128)–(1008,671)` on a 1280×800 canvas.
- **A driven run reaches gameplay and the user validated the picture by eye** (2026-11-09, PAL
  `GZ2P01.iso`, run `pr_`, frames 2,150–4,800): Link's lit, textured face with a dialogue box; two
  characters with the HUD; an outdoor scene with distance fog, terrain, a riverbank, foliage and a
  mounted rider. Consecutive frames differ by 34–39% of pixels, so the scene is live. Draw counts are
  215–376 per frame. This is the artefact the old §7.4 wanted a desktop reference render for.
- Zero failing allocations, zero crash markers, zero `GL error uploading` warnings on a clean run.
  `JKRExpHeap::create` succeeds repeatedly with ~21.9 MiB free in `gameHeap`.
- The guest arena is **64 MiB** (`67092240` bytes). Until 2026-11-02 the retail GameCube trim
  silently removed 24 MiB from it every run (§7.1).
- All host suites pass (§6).
- Vertex decode is measured separately from the frame at 3.95–6.45 ms/frame in Vita3K (§7.9). The
  content-addressed decode cache is correct, hits 74–76%, and is **off**: too small a share to measure here.
- A fatal path **names itself correctly**. `abort()` is intercepted link-time; every death writes
  `ux0:data/tpvita/crash.log` with the caller resolved to a symbol (§7.10, §7.11). Proven by a
  deliberate abort recording `abort_caller=0x81009bab main`, checked against `nm`.

**What this does not prove.** Vita3K is an emulator; nothing here has run on hardware. Audio is
disabled (`DUSK_AUDIO_DISABLED`). Save RELOAD, the pause menu and most of the game have never been
exercised. The accurate claim is "boots, renders and reaches gameplay", not "playable".

**How the picture has been checked.** Twice, both by the user by eye, both non-pixel: (1) 2026-11-05
the title screen matched a reference frame after the §3d.3 scale fix; (2) 2026-11-09 the in-game
frames above. Treat these as "the picture looks right", not "every 3D effect is proven".

---

## 3. Environment and commands

```sh
export VITASDK=/home/yanserm1/vitasdk
export PATH="$VITASDK/bin:$PATH"
```

### Build the VPK

```sh
cd /home/yanserm1/psvita/tp-vita
cmake -S . -B build            # ALWAYS, see trap 2
cmake --build build --target tpv_vita-release.vpk-vpk -j"$(nproc)"
```

**Two silent traps.**

1. **The target is `tpv_vita-release.vpk-vpk`, not `tpv_vita-release.vpk`.** The wrong name prints
   nothing, exits **0** and builds nothing. Check the VPK mtime, not the exit code
   (`cmake --build build --target help | grep vpk` lists the real name).
2. **The aurora tree is only re-synced at configure time.** After editing `overlay/**` or `patches.py`,
   a plain build reuses the previous tree. This cost a session: a restored fix was still failing
   because `build/aurora_lib/` held the reverted copy. Always run `cmake -S . -B build`, then `grep`
   the regenerated tree to confirm the change landed.

Fresh configure, only if CMake files changed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTPV_PACKAGE=ON
```

### Run in Vita3K (headless, in podman)

```sh
VITA3K_DIR=/home/yanserm1/vita3k-linux TPV_V3K_WORK=/home/yanserm1/vita3k-work \
  ./tests/vita3k/run.sh run <interval_seconds> <num_screenshots>
```

- **The first seconds of any run produce blank frames.** `40 2` gives two black PNGs; `75 4` gives
  real ones. Do not conclude "rendering is broken" from a short run. A blank screenshot is
  `mode=1`, 263 bytes; a rendered one is `mode=RGB`, tens of KB.
- Screenshots land in `$TPV_V3K_WORK/out/shot_N.png`, log in `$TPV_V3K_WORK/out/vita3k.log`, guest
  trace in `…/ux0/data/tpvita/dvd_trace.txt`.
- **Delete `dvd_trace.txt` before each run**, or old and new lines are indistinguishable.
- The harness prefers the dev VPK when present; `TPV_VPK=` overrides.

### Analyse screenshots without vision

Decode them numerically:

```sh
python3 -c "
from PIL import Image
import numpy as np, glob
for f in sorted(glob.glob('/home/yanserm1/vita3k-work/out/shot_*.png')):
    im = Image.open(f); a = np.array(im.convert('RGB'))
    cols = len(np.unique(a.reshape(-1,3), axis=0))
    nz = np.argwhere(a.any(axis=2))
    bbox = None if nz.size==0 else (int(nz[:,1].min()), int(nz[:,0].min()), int(nz[:,1].max()), int(nz[:,0].max()))
    print(f.split('/')[-1], im.size, 'mode='+im.mode, 'colours=%d'%cols, 'bbox=%s'%(bbox,), 'mean=%.1f'%a.mean())
"
```

PIL 12.2.0 and numpy 2.4.6 are available. Healthy output is `mode=RGB`, thousands of colours,
`bbox=(271, 128, 1008, 671)`.

That bbox is derived, not arbitrary: the EFB is 608×448 scaled by 1.2143 to 738×544, placed at
`glViewport((960-738)/2, 0, 738, 544)` = `(111, 0)`, and the Vita surface sits 1:1 inside the
1280×800 window at `(160, 128)`. Hence `160+111=271`, `128+0=128`, `271+738-1=1008`,
`128+544-1=671`. **Pixel-exact and measured.** A future run that disagrees means `present()` changed.

---

### 3b. Build variants: what ships vs what you debug with (2026-11-03)

```
tools/build.sh release   -> build/tpv_vita-release.vpk      -O3, NDEBUG,   title TPLG00001
tools/build.sh dev       -> build-dev/tpv_vita-dev.vpk      -O1, asserts,  title TPLG10001
tools/build.sh debug     -> build-debug/tpv_vita-debug.vpk  -O0, asserts,  title TPLG20001
```

Use `dev` to reproduce a failure, not `-O0`: this runs in a slow emulator, and an unoptimised build
can drop below the frame rate where an intermittent bug appears. `-O1` keeps frame pointers.

**The invariant: struct layout must not differ.** CMake's Debug flags inject `-DDEBUG`, and dusklight
adds members under `#if DEBUG` (`d_meter_HIO.h`, `f_ap_game.h`, `d_map.h`, …), which changes
`sizeof()` of game objects. A dev build would then be a *different program* from the one that ships.
`-DDEBUG`/`-D_DEBUG` are stripped in every variant and `DEBUG` is pinned to `0`, verified by
preprocessing `dusk_pch.hpp`: `debug_is_on = 0`. `PARTIAL_DEBUG=1` is the safe half and is on everywhere.

**Three traps found while wiring this, all silent.**

1. **The toolchain owns the build-type flags.** `vita.toolchain.cmake` sets
   `CMAKE_CXX_FLAGS_DEBUG="-O0 -g -DDEBUG -D_DEBUG"`, and build-type flags are appended *after*
   `add_compile_options`, so a variant flag added there loses. That is also where `-DDEBUG` came from.
2. **A custom build type drops every flag.** CMake reads `CMAKE_CXX_FLAGS_<UPPERCASE TYPE>`, so
   `CMAKE_BUILD_TYPE=Dev` reads an empty `CMAKE_CXX_FLAGS_DEV`: the cache held the right value while
   the compile line had no `-O` at all.
3. **`add_compile_options` does not split on spaces.** `"-O1 -fno-omit-frame-pointer"` arrives as one
   argument → `cc1plus: error: argument to '-O' should be a non-negative integer`. Use `separate_arguments`.

Resolution: build-type flags are cleared to empty and one `add_compile_options` list is the only
authority. One place to read, no ordering question.

**Smaller choices worth keeping.** Distinct title IDs and VPK names, so installing the dev build does
not overwrite release and a frame count stays attributable (`00001`/`10001`/`20001`; the ID must be
4 uppercase letters + 5 digits and `check_vpk.py` enforces it). `boot.log` states its own variant.

### 3c. The intermittent death is a borealis FATAL — proven, not inferred (2026-11-03)

The dev build reproduced the crash on its first run and the §7.11 recorder named it:

```
=== abort() from abort() ===
abort_caller=0x81a8fbbb  borealis::log::detail::write_fatal(std::basic_string_view<char, ...>, ...)
```

So the earlier guesses (`PPCHalt`, `OSSwitchFiber`, config double-registration) were wrong for this
crash: it is a `LogLevel::Fatal` raised through borealis. The **message** was still missing, which is
why `dusklight/src/dusk/logging.cpp` installs `Options::onFatal`: it writes to stderr and to
`ux0:data/tpvita/fatal.txt`. The hook is necessary because borealis, when its logger is not yet
initialised, pushes a fatal onto `state.bootstrap`, and that queue is replayed **only inside `init()`** —
a fatal after init begins is otherwise written nowhere. `onFatal` runs in both branches, before `abort()`.

**14 consecutive release runs then produced no fatal at all** (8 × 60 s, 3 × 150 s, 3 × 240 s reaching
frame 7,200–7,500), with no `crash.log`, no `fatal.txt` and no guest exception. That retires the old
"roughly 1 run in 7" figure, which was measured on an older tree. Either an intervening change fixed
it (§7.1's arena fix is the plausible candidate) or the rate is low enough that 14 runs prove nothing.
**Both readings fit the data; neither is proven.**

### 3d. The idle runs never left the attract loop — and the game stopped at a memory-card prompt

Idle runs do not play the game. Their twilight-sky frames are the **title/attract loop**, so new scene
data is never loaded, and the data-dependent fatal sites cannot fire:

- `gx.cpp:161` calls `comp_cnt_count()`, which `fatal()`s on an unsupported `GXAttr`/`GXCompCnt` pair;
- reached per display list from `dl.cpp:62` and `command_processor.cpp:366`.

New geometry with new vertex attribute formats is what moving through the game produces, and 7,500
idle frames on one screen cannot produce it. "14 clean runs" is therefore weaker evidence than it looks.

To drive the game, `tests/vita3k/in_container_script.sh` replays a fixed action script (`run.sh script
<file> [prefix]`). Its honest limit: the **key sequence is fixed, the timing is not** — presses are
wall-clock, so a press can land on a different frame each run. That is a reproducible sequence, not a
reproducible run. Frame-counted injection needs a hook inside the app (§3d.5).

Input does reach the game: a tap moved the screen from "No Memory Card found in Slot A" to "Continue
without saving?" with a cursor, so SDL3 → `aurora::input` → `PADRead` works. But every route returned
to the same check, a **self-perpetuating loop**: `mDoMemCd_Ctrl_c::getStatus` returned
`CARD_STATE_NO_CARD_e`, `MemCardStatCheck` showed `ERRMSG_WAIT_KEY` then `NO_SAVE_SEL_DISP`, and
answering it re-ran the same check. The "Checking Memory Card in Slot A…" header is permanent chrome
on that screen, not a status.

Ruled out by test: that the card path's parent was the disc image. Creating `ux0:data/tpvita/EUR/Card A`
by hand changed nothing — same shots, same md5, directory still empty. **The cause was elsewhere (§3d.1).**

### 3d.1 The cause was a dangling path, not card logic (2026-11-05)

`AuroraConfig`'s path strings are borrowed pointers that upstream copies with `strdup()`; the Vita
backend (`vita/aurora/backend.cpp`) stored them raw. Dusklight builds them from `std::string`
temporaries inside a block of `game_main()`, the block ends, and the buffers are freed and reused.
`g_config.userPath` was read later by `CARDInit()` and had become the **disc image path**, so the card
folder resolved to `ux0:data/tpvita/GZ2P01.iso/EUR/Card A`. Creating a directory under a regular file
fails with ENOENT, `CardGciFolder::open()` left `m_error = NOCARD`, `CARDCheck()` reported `NOCARD`,
and `MemCardStatCheck` looped forever — from a place nowhere near the card code. The evidence is the
emulator log: before the fix `Missing file at .../tpvita/GZ2P01.iso/EUR/Card A`, after it
`.../TwilitRealm/Dusklight/EUR/Card A`.

**Fix.** `copy_string()` in `vita/aurora/backend.cpp` takes owned copies into static storage, refuses
a path over 512 bytes rather than truncating it into a *different* directory, and logs the resolved
paths on every boot:

```
paths: user 'ux0:/data/TwilitRealm/Dusklight/', cache '...', resources 'app0:/', app 'Dusklight'
```

**A second, independent cause.** Even with the path fixed the card directory was never created:
`_sceIoMkdir` on `.../Dusklight/EUR/Card A` returned `0x80010002` (ENOENT) because the parent `EUR`
did not exist. `aurora::io::create_directories()` calls `SDL_CreateDirectory()`, *documented* as
creating missing parents — but that is per-platform. The shipped library settles it:
`SDL_SYS_CreateDirectory` in `SDL_sysfsops.c.obj` is a bare `mkdir` (compare `SDL_SYS_GetPrefPath`,
which does walk and create each component). On Vita the header's promise is not kept.

`create_directories()` now builds the path component by component and **skips a component ending in
`:`** — `SDL_CreateDirectory("ux0:")` fails and cannot even be stat'd (`Failed to translate path:
ux0:`), turning the whole call into a failure on its first iteration. The skip is required, not an
optimisation.

**Verified in Vita3K.** With `…/Dusklight/EUR` deleted first, a completed run leaves
`TwilitRealm/Dusklight/EUR/Card A` created by the game, so `CARDInit` formats the GCI folder and the
card channel comes up: the first live memory card this port has had.

**The lesson.** The first attempt edited `extern/aurora/lib/io.cpp` directly — upstream source. It
compiled, produced a VPK and changed nothing; two emulator runs concluded "the fix did not work". The
missing half of the §8 rule is that editing *upstream* `extern/aurora/lib` is as ineffective as
editing `build/aurora_lib`.

### 3d.2 The memory-card prompt no longer blocks play, and saves are written (2026-11-05)

`shot_5` of the scripted run showed an in-game scene while the card channel still reported `NOCARD`,
so "cannot be played past file select" was a consequence of the §3d.3 scale bug (which drew the dialog
off-screen), not of the card state.

**Correction:** that shot was described here as "a lit stone bridge over a dark cavern" — a reading of
a picture nobody compared to anything. What is defensible is narrower: the prompt was answered, a GCI
was written, and the game advanced **into the save-creation sequence**, where it stayed on the name
grid for eleven screenshots. Only the pixels count.

**Saves work, write path.** A frame-counted run answered "Yes" to "Would you like to create a save
file … in Slot A?" and the game wrote a real GCI:

```
TwilitRealm/Dusklight/EUR/Card A/01-GZ2P-gczelda2.gci   32,832 bytes
header: 47 5a 32 50 30 31 00 05 "gczelda2"      (GZ2P01, 5 blocks, filename field)
10,497 non-zero bytes — real data, not a zero-filled stub
```

The run continued into the new-game intro ("Enter horse name" → "Epona") and "Enter name" → "Link".
**Save RELOAD is untested**: a second run with the GCI in place was driven the same way and began a
*new* game rather than continuing. Whether that is intended for a card never loaded in that session,
or a defect in the load path, is not determined — the file-select screen itself was never captured.

### 3d.3 Why the title screen was cut, and the rects to measure next time (2026-11-05)

The reported symptom: the logo filled the frame and "PRESS START" plus the copyright line were
missing. That is a **render-scale bug**, not a scene bug.

`ensure_efb()` allocated the EFB at 75% (608×448 → 456×336), but `main_target_size()` returned the
**logical** size and `get_render_target_size()` returned `main_target_size()` for the main target, so
`gx::map_logical_viewport()` computed `target / logical` = `608/608` = **1.0**. The 75% was applied to
nothing, so every full-screen 608×448 viewport was issued against a 456×336 FBO: the picture was
magnified by 1/0.75 and cropped.

Measured before the fix:

```
EFB target: logical 608x448 -> 456x336 (75%), present 960x544
rect on 456x336: viewport (0.0,0.0) 608.0x448.0  scissor (0,0) 608x448
```

**Fix.** `efb_target_size()` (in `vita_frame.hpp`, defined in `vita_gl_replay.cpp`) returns the scaled
size; `get_render_target_size()` reports it for the main target; `ensure_efb()` allocates from the same
function so the two cannot drift. After:

```
rect on 456x336: viewport (0.0,0.0) 456.0x336.0  scissor (0,0) 456x336
```

**Keep the instrumentation.** `apply_viewport()` logs each distinct (target size, viewport, scissor)
once per process and `ensure_efb()` logs the logical → EFB conversion. Two log lines settled in minutes
what prose had guessed wrong.

### 3d.4 What the 2026-11-05 changes were tested against, and what was not

| Suite | Result at the time |
|---|---|
| `crash` | PASS (7 checks; now 19) |
| `texture` | 3,780 checks, 0 failures (the one MISMATCH printed is the deliberate negative control) |
| `disc` | `test_probe` 14/0, `test_diag` 10,686/0 |
| `vertex` | **71,063 checks, 0 failures** |
| `e2e` | **0 scenes failed** (87 scenes then, 91 now) |

Both suites had been **broken**, and both breaks removed tests silently:

- `vertex` could not link: `undefined reference to aurora::log_internal`, then `aurora::g_config`. The
  decoder reports through `aurora::Module`, which needs `logging.cpp` plus a `g_config` definition.
  Fixed by adding `logging.cpp` and `tests/host/vertex/stubs.cpp` to `run.sh`.
- `e2e` could not compile: `SDL3/SDL_events.h: No such file or directory`. The header **was** present,
  in `$VITASDK/arm-vita-eabi/include/SDL3`; the header-symlink lists in `tests/host/build.sh` and
  `tests/host/e2e/run.sh` named only `GLES2 KHR EGL fmt`. The merged tree reaches it through
  `aurora/event.h`. Adding `SDL3` to both lists fixed it. "This machine has no SDL3" was wrong.

**`e2e` had in fact been red since the 75% scale was introduced (§7.7).** Measured: with the four
viewport/scale files stashed, **84 of 87 scenes failed**; with them applied, 30; after the scale fix,
**0**. Nothing was run in between, so the suite that exists to catch this class of error was not
consulted when the error was added.

The remaining 30 were one cause: the suite asserts exact pixels (a 1-px line, an 8-px point, a scissor
edge, a TEV colour), and at 75% those are filtered — a full-intensity line came back `(15,210,15)`
instead of `(0,255,0)`. Rendering the **host** suite at 100% removes the mismatch at its source instead
of loosening tolerances, which would hide real regressions. `kEfbScalePercent` is `75` on `__vita__`
and `100` elsewhere; the device still logs `logical 608x448 -> 456x336 (75%)` with viewport
`456.0x336.0`, so the scaled path is what hardware runs and the unscaled one is its reference.

Not verified on any target: a save written and reloaded, audio, performance, anything on hardware.

### 3d.5 Frame-counted input: `vita/src/input_script.cpp` (2026-11-05)

The injector counts **frames** instead of seconds. It is one new file plus three lines of wiring and
needed **no change to dusklight**:

| Piece | Where |
|---|---|
| script parser and pad push | `vita/src/input_script.cpp` |
| hooked once per frame, before the game reads the pad | `vita/aurora/backend.cpp` `aurora_update()` |
| loaded at boot, logged loudly | `vita/src/main.cpp` |
| image content | `TPV_VITA_SOURCES` in `CMakeLists.txt` |

The hook point matters: `aurora_update()` runs once per loop iteration and **before** the game's
`JUTGamePad::read()` → `PADRead()`, so a push there is what the game sees on that frame. Nothing in the
game was touched, which keeps this mergeable with upstream.

Format (`ux0:data/tpvita/input_script.txt`; absent = off, zero cost):

```
# comment
<frames> <name>[,<name>...]     hold for N frames
<frames> .                      hold nothing for N frames
```

* **Buttons** (`PADStatus::button`): `A B X Y START LEFT RIGHT UP DOWN L R Z`
* **Stick** (`PADStatus::stickX/stickY`): `STICKLEFT STICKRIGHT STICKUP STICKDOWN`

`LEFT` and `STICKLEFT` are **not** the same input, and this is the whole lesson: Dusklight's menus read
the stick, not the D-pad, through `STControl::checkLeftTrigger()`, which needs a nonzero `stickValue`
and derives direction from `stickAngle`. A script that pressed `LEFT` set `PAD_BUTTON_LEFT`; nothing on
that screen reads that bit, so the cursor never moved, `A` confirmed "No" again, and the game looped.
Reproduced twice before the cause was found.

**Full deflection, and why a smaller push is a trap.** `PADClamp`'s `ClampStick` subtracts a dead zone
of 15 and rescales by `72/(127-15)`, so a raw 100 becomes 54, not 100. `CStick::update` divides by 54,
so 54 → 1.0, just over the 0.9 threshold `dFile_select.cpp` uses. A raw 90 becomes 45, 45/54 = 0.83,
and the cursor never moves while the log looks perfect. Under `EClampCircle` the divisor is 56/38. Full
deflection (127) saturates to 1.0 under every clamp, so it is the only value worth using.

**Three defects the first version had**, each found by measurement: trailing `#` comments were not
stripped, so `60 .   # settle` rejected the WHOLE script; newlib's `vsnprintf` has no `%zu`, so the
summary printed a literal `zu` and consumed no argument, shifting every later value; and there was no
stick output at all.

**The one honest limit.** Frame numbers are exact; the SCREEN at a frame is not. The card prompt is a
~28-state machine (`MemCardCheckMain`) with variable-length animations, so one precisely-timed press is
the wrong shape — one landed on a plain message with no cursor and was lost. The scripts therefore use
a **sweep** that alternates a cursor move with `A`: every branch either advances or re-enters the sweep.

**Output.** `[input script]` lines go to `ux0:data/tpvita/input_script.log` and to stderr. The file is
the readable part: Vita3K does not forward guest stderr into `vita3k.log` (measured), and on hardware
there is no log at all. Every step is logged with its frame, so a run can be checked against its script:

```
[input script] frame 1050 -> buttons 0x0000 stick -127,0 for 30 frames (30 STICKLEFT)
```

```sh
w="$HOME/vita3k-work/home/.local/share/Vita3K/Vita3K/ux0/data/tpvita"
cp tests/vita3k/scripts/frame_card_sweep.txt "$w/input_script.txt"
VITA3K_DIR=... TPV_VPK=build/tpv_vita-release.vpk \
  tests/vita3k/run.sh script tests/vita3k/scripts/waits_sweep.txt sw_
```

Use the `waits_*.txt` harness scripts, not the key-pressing ones: with the injector active, a host key
press is noise that changes the result.

**Proved here (Vita3K only):** the title screen renders uncropped; `START` advances it; the card prompt
is answerable; "Yes" writes a real 32,832-byte GCI; the game reaches the naming screens and gameplay
(§3d.6). **Not proved:** save reload, audio, performance, hardware.

### 3d.6 The name screen exits correctly, and the picture reaches real gameplay (2026-11-09)

**Headline: the port renders actual Twilight Princess gameplay.** Run `pr_`, frames 2,150–4,800 of a PAL
disc, 16 valid screenshots: Link's face lit and textured with a dialogue box (`pr_13`), Link and another
character with the HUD (`pr_15`), and an outdoor scene with distance fog, terrain, a riverbank, foliage,
a rock outcrop and a mounted rider (`pr_16`). Frames change 34–39% pixel-wise, so it is live. Draw
counts 215–376/frame. **The user validated this by eye**; it replaces the de-scoped desktop reference render.

The screen is `dName_c` with a nine-entry `mSelProc` state machine (`include/d/d_name.h`):

```
proc: 0=MOJI_SELECT 1=MOJI_SEL_ANM 2=MOJI_SEL_ANM2 3=MOJI_SEL_ANM3
      4=MENU_SELECT 5=MENU_SEL_ANM 6=MENU_SEL_ANM2 7=MENU_SEL_ANM3 8=WAIT
menu: 0=HIRA 1=KATA 2=EIGO 3=END
```

A `[name-proc]` probe in `dName_c::_move()` (one line per distinct `(proc, menu, row, col)`) plus a
`[name-press]` probe gave, on a clean run:

```
proc=0 menu=0   cursor walking 12->11->...->0, wrapping
[name-press] START delivered in proc=0 menu=0
proc=2 menu=2   <- MOJI_SEL_ANM2, mSelMenu set to EIGO
proc=4 menu=2   <- MENU_SELECT, cursor on EIGO ("ABC")
[name-press] A delivered in proc=8 menu=2   <- PROC_WAIT
```

**`proc=8` after A is the proof of success.** A in `MenuSelect()` calls `menuAbtnSelect()`, which on a
PAL disc jumps to `pal_eigo:`, calls `nameCheck()`, and on a non-empty name sets `mSelProc = PROC_WAIT`
and `mIsInputEnd = 2`; `dFile_select_c::nameInput()` then calls `dComIfGs_setPlayerName()`. There is a
second identical screen for the horse name (`nameInput2()`).

On this PAL disc `EIGO_OR_END` resolves to `MENU_EIGO` (2), **not** `MENU_END` (3), because
`DUSK_IF_ELSE((isRegionPal() ? MENU_EIGO : MENU_END), MENU_END)`. So the menu shows `ABC` already
selected and A confirms there — the highlighted `ABC` was correct, not a bug.

**The exit, read from the source:**

| Input | State | Effect | Where |
|---|---|---|---|
| `A` | `PROC_MOJI_SELECT` | **types** the highlighted letter | `d_name.cpp:1211` → `menuAbtnSelect()` |
| `START` | `PROC_MOJI_SELECT` | moves the **menu cursor** (ABC/abc/END) to END | `d_name.cpp:395` |
| `A` | `PROC_MENU_SELECT` + END | `nameCheck()` + `menuAbtnSelect()` → `mIsInputEnd = 2`, screen ends | `d_name.cpp:1211` |

So the exit is a **two-press pair, START then A**; neither alone leaves. There are **two** name screens
(`d_file_select.cpp:1747` player, `:1845` horse), so the pair repeats. A repeating `A` cadence is a
repeating typist: it typed five stray characters into a full field, producing the frozen `LinkJF` screen.

**Two diagnoses were wrong and are recorded so they are not repeated.**

1. *"A types letters, so the screen is a dead end."* A does type in `PROC_MOJI_SELECT`, but the screen
   also moves on. The `LinkJF` text was typed by the sweep, not evidence of a trap.
2. *"START then A is ignored, the screen is stuck at `proc=4`."* A **was** delivered and did exit.
   The frames looked identical because of **sampling time, not game state**: three "frozen" shots were
   taken at 6–9 minutes, before the exit at frame ~2,150–2,430. Two screenshots taken seconds apart
   showing the same pixels are evidence about *when they were taken*. A state probe settled it, not
   another look at the picture. The 8,260-frame run's 16 screenshots were named `g_naming_1 … o_gameplay_3`;
   the names were **hopes**, and the pixels differed by 0.09–0.28% across eleven of them.

**The PLAY-scene phase chain.** `d_s_play.cpp:1660` logs one line per distinct `(phase index, returned
cPhs state)` pair from `dScnPly_Create`, reading `field_0x1c4.id` **before** the handler runs, so a phase
that is never entered cannot be confused with one that succeeds:

```
index=0 -> state=0   INIT/retry   phase_00  (resetGame)
index=2 -> state=0   INIT/retry   phase_1_0 (syncStageRes "Stg_00")
index=5 -> state=0   INIT/retry   phase_2   (object res)
index=6 -> state=0   INIT/retry   phase_3
index=6 -> state=4   COMPLEATE    phase_3 finished
```

Every retry resolved; nothing sat in `cPhs_INIT_e`. Keep the probe: ~15 lines, one line per
`(phase, state)` pair, and the only thing that answers "which create phase is the play scene in"
without a debugger.

**Not established.** One earlier run collapsed — draws `443 -> 1` at window 1920, `sResolveSeen` frozen
at 19,870, wall time pinned to 33.33 ms/frame (the §7.8 trap-3 signature) — and this document once
called that the play scene never starting. **It has not reproduced** and no change was made to fix it.
Do not write "the stall is fixed". Two things it did establish, both still true: it was **not a crash**
(the loop ran to frame 8,990 at 30 Hz, `boot.log` complete, injector finished cleanly), and **`ret=4` is
success** — `cPhs_COMPLEATE_e`, which `cDyl_LinkASync` returns unconditionally on `TARGET_PC`, so that
`[DIAG]` line is noise on this build.

---

## 4. What was done in the first sessions

### 4.1 Aurora upgrade attempt, abandoned and fully reverted

The attempt re-vendored aurora to `505b5eb`. It exposed a hard blocker.

**The blocker:** aurora `505b5eb` **removes 22 public PAD functions** from `include/dolphin/pad.h` for a
new `aurora::input` binding. Dusklight uses the old API in 8 files across 63 usage lines
(`ui/controller_config.{cpp,hpp}`, `ui/input.cpp`, `menu_pointer.cpp`, `ui/overlay.cpp`,
`ui/string_button.cpp`, `ui/command_console.cpp`, `m_Do_main.cpp`, `d/d_drawlist.cpp`).

**The trap:** it is tempting to conclude "dusklight is behind, nothing to pull". Wrong.
`git ls-tree HEAD extern/aurora` in dusklight returns `3227d76…` — **dusklight pins `3227d76`
explicitly**, so upstream did not move on without it. **No upstream branch migrates.** Do not re-open
this without first checking whether dusklight itself has migrated.

**Also:** upstream aurora rewrote its history, so `3227d76` is an orphaned snapshot. A raw `git diff`
across that boundary conflates the whole divergence and is misleading.

### 4.2 The revert, proven rather than assumed

Done by reconstruction and **byte comparison**, since the merged tree has no history: checkout
`pre-upgrade-3227d76`; extract the pre-upgrade merged tree from
`.snapshot-pre-aurora-upgrade-20261029/aurora_lib-merged.tar.gz`; diff against upstream
(`13` modified files, `12` overlay-only); rewrite `patches.py` from those diffs; drop the
`505b5eb`-only `readback/` subsystem; restore `gx/texture.cpp`, `cmake/aurora.cmake`,
`cmake/aurora_gx.cmake` and `vita/aurora/backend.cpp`; assemble; compare.

**Result: PASS — `patches.py` + overlay reproduce the known-good tree byte-for-byte** (178 files),
after fixing three cosmetic mismatches (a dropped `m_cv.notify_all()`, `std::lock_guard lk{m_mutex}` vs
`lk(m_mutex)`, one lost indentation level). **Reuse this technique** for any patch-set change: rebuild,
extract the known-good snapshot, `diff -rq`.

### 4.3 Cherry-pick audit of the 7 aurora commits

All seven: **Skip.**

| Commit | What it is | Verdict |
|---|---|---|
| `8e356df` | `GXPeekZ` | already exists at `3227d76` in `GXCpu2Efb.cpp` |
| `afd4bb8` | `GDLoadTexObj` / `GDLoadTlut` | nothing we compile calls them; the game uses `J3DGDLoadTlut` |
| `f8347d9` | FIFO processing-mode ergonomics | our 1-line `patches.py` substitution (`ProcessingMode::Thread` → `Drain`) already achieves it |
| `12943d1` | WebGPU-only | irrelevant to a GLES2 backend |
| `d0ace68` | the PAD API removal | the breaking change (§4.1) |
| `7c8c611`, `505b5eb` | depend on `d0ace68` | cannot be taken without it |

### 4.4 Bring-up: heap bugs found and fixed

**Bug 1 — root heap short by 480 bytes, causing `SIGABRT`.** The guest died just before
`dvdHeap created`. Symbolising the fault PC gave `_kill_r` with **r2 = 6** (SIGABRT), which proves
`abort()`, not a null dereference — **do this check first**. Tracing into `JKRExpHeap::do_alloc` showed
the `TARGET_PC` error path runs `CRASH(...)` → `OSPanic` → `Log.fatal` (`[[noreturn]]`), so **a failed
allocation is fatal, not recoverable**. Instrumented: `do_alloc: size=0x10000 align=16 heap=Root
free=0xFE20` — 65536 requested, 65056 free.

Root cause: `arenaSize` is created as a **child** of the root heap, so it cannot take the whole arena,
and the root heap must still cover the dvdHeap plus `JKRExpHeap`/`CMemBlock` overhead. Fix in
`m_Do_machine.cpp`, rounded generously because `sizeof(JKRExpHeap)` varies with compiler and the
failure mode is a hard abort:

```cpp
constexpr u32 VitaRootReserve = 0x30000;   // 192 KiB
u32 reserved = archiveHeapSize + j2dHeapSize + gameHeapSize + commandHeapSize +
               dbPrintHeapSize + VitaRootReserve;
```

**Bug 2 — game heap exhausted at 17 MiB.** With bug 1 fixed `mDoMch_Create()` returned 1 for the first
time, then hit `do_alloc: size=0x9AF30 heap=gameHeap free=0x0`. Measured: 57 allocations totalling
167 MiB, largest `0x108DD20` (16.55 MiB), while the System/zelda pool held ~8 MiB idle.

```cpp
archiveHeapSize = 0x8E0000;  // 8.875 MiB — the un-doubled GameCube value
j2dHeapSize     = 0x7D000;   // 500 KiB — the un-doubled GameCube value
gameHeapSize    = 0x1700000; // 23 MiB, taking 6 MiB from the idle System/zelda pool
```

The desktop build multiplies by 20 (~85 MiB), which cannot exist in a ≈40 MiB arena, and this is a
32-bit build like the GameCube, so "×2 for 64-bit pointers" does not apply. Result: 0 failing
allocations, main loop reached, frames rendered.

**Bug 3 — `JKRExpHeap::create` computed `dataPtr` before checking the allocation.** `dataPtr` was
computed from `memory` before `if (!memory) return NULL;` — undefined behaviour on failure, which GCC
can fold. Reordered, with a `// NOTE:` explaining why the order matters.

### 4.5 Trace instrumentation: what is kept and what was deleted

A `fopen`/`fprintf`/`fclose`-per-call trace exists because a guest abort discards buffered stdio.

**Kept** (one-shot boot staging, cheap): `mch_trace` (`m_Do_machine.cpp`, 40 sites), `m01`
(`m_Do_main.cpp`, 6), `jfw_trace` (`JFWSystem.cpp`, 20), `aram_trace` (`JKRAram.cpp`, 8), `exp_trace`
(`JKRExpHeap.cpp`, 5 — failure path plus `create` entry/exit).

**Deleted** (recurring or stale): the per-frame `loop frame N` counter, the per-frame `present()` trace,
and all four `tpv_dvd_trace` patches in `patches.py`.

---

## 5. Verified facts worth not re-deriving

### 5.1 EFB resolution is 608×448 and that is correct

`m_Do_graphic.h` defines `FB_WIDTH_BASE 608`, `FB_HEIGHT_BASE 448`, and the 640×456 widescreen variant
is `WIDESCREEN_SUPPORT && !TARGET_PC`. A `TARGET_PC`/Vita build therefore correctly gets 608×448, and
`dusk/defaultWindowWidth/Height` are 608/448. **Not a Vita-specific deviation; do not "fix" it.**

### 5.2 Present geometry is correct

`present()` in `vita_gl_replay.cpp` aspect-fits, then centres with `glViewport`. With a 960×544 surface
and a 608×448 EFB this yields `738×544 at (111, 0)`, and the measured bbox `x271..1008 y128..671` matches
**exactly**. Pillarboxing is symmetrical (111 px each side). The large black area is expected: 738×544
within 1280×800 is 39.2% predicted vs 39.26% measured.

### 5.3 `check_vpk.py` passes

```
info: ATTRIBUTE2=0xC (+109 MiB main memory)
info: TITLE_ID=TPLG00001 APP_VER=01.00 TITLE='Twilight Princess' CATEGORY='gd'
8 entries; 0 error(s), 0 warning(s)
```

The script ships with the `vita-port` skill, at `~/.roo/skills/vita-port/scripts/check_vpk.py`
(also mirrored under `~/.claude`). `README.md` uses the `~/.roo` path.

### 5.4 `present_target` and `OSStopwatch` are unused

Neither is referenced by anything compiled from the merged tree. Do not add them to a source list to
"fix" a link error.

### 5.5 `JFWSystem::init()` exception wrapper is diagnostic only

`m_Do_machine.cpp` wraps it in `try`/`catch` that records `what()` and the `std::system_error` code, then
**re-throws**. It earns its place: the original abort was an uncaught `std::system_error` from a context
no handler could catch, and the wrapper named the failing call.

---

## 6. Host test suites — all green

Each suite has its own `run.sh`; run them from `tp-vita/` with `VITASDK` set.

| Suite | Command | Result |
|---|---|---|
| vertex | `./tests/host/vertex/run.sh` | 71,063 checks, **0 failures**; 4,000 random formats vs reference |
| shader | `./tests/host/shader/run.sh` | 2,000/2,000 configs linked on Mesa, **0 failed**; segment-table, u32-lane, light-slot and matrix-bank violations all 0 |
| texture | `./tests/host/texture/run.sh` | 3,780 checks, **0 failures** (negative control still detected) |
| disc | `./tests/host/disc/run.sh` | 14 + 10,686 checks, **0 failures** |
| crash | `./tests/host/crash/run.sh` | 19 assertions against the real generated table |
| printf | `./tests/host/check_printf_formats.sh` | clean — no `%z`/`%j`/`%t`/`%hh` in `vita/` |
| e2e | `./tests/host/e2e/run.sh` | **91 scenes, 14,266 pixel checks, 0 failed**; `TPV_E2E_ASAN=1` for AddressSanitizer |

The shader suite **reports** configs exceeding Vita uniform limits; that is its purpose, not a failure.
Current maxima: `fsVec4=26`, `vsVec4=153`, `maxAttribs=19`; over the caps: 93 configs with fs>16,
104 with vs>128, 4 with attribs>16.

Do not hand-build a link line for a suite: each `run.sh` already does it with its own stubs. That
detour was wasted once already.

---

## 7. Open problems

### 7.1 Heap budget — the retail arena trim was live, and is now gone (2026-11-02)

**Status: the size arithmetic is sane; the `0xBF18265F` allocation is still unexplained.**

One run in seven used to fail with

```
JKRExpHeap: do_alloc FAILING size=0xBF18265F align=32 heap=archiveHeap
            heapsize=0x8DFF50 used=0x6B4678 free=0x22B794
```

`0xBF18265F` is 3.0 GiB against 2.27 MiB free — a **corrupted size argument, not exhaustion**.

**The separate bug found here.** `mDoMch_Create()` ran the **retail GameCube arena trim**, unguarded:

```cpp
uintptr_t arenaHi = (uintptr_t)OSGetArenaHi();
if (arenaHi > 0x81800000 && arenaHi - 0x1800000 > arenaLo)
    OSSetArenaHi((void*)(arenaHi - 0x1800000));
```

`0x81800000` is a physical GameCube address. A Vita process relocates the guest arena, so the comparison
describes a machine that is not there — and **it fired anyway**:

| | `arena lo` | `arena hi` | `size` | MiB |
|---|---|---|---|---|
| with the trim (old) | `0x8314c1c0` | `0x859481c0` | 41926416 | **40** |
| trim compiled out | `0x8314c1a8` | `0x871481a8` | 67092240 | **64** |

The delta is exactly `0x1800000` = 24 MiB. Every run until now gave the game an arena 24 MiB smaller
than the platform provides, and the heaps carved from it (§4.4) were sized against a figure the hardware
never returns. Fixed with `#if !DEBUG && !TARGET_VITA` and the measurement in the comment; verified that
`0x81800000` no longer appears in `mDoMch_Create` and that the trace reports 64 MiB.

**Still open, and must not be recorded as explained.** Ruled out by arithmetic:
`daMP_THPPlayerCalcNeedMemory()` terms are all `ALIGN_NEXT(..., 32)`, so its result is a multiple of 32,
while the observed value has `& 0x1F == 0x1F`. As IEEE-754, `0xBF18265F` ≈ -0.594, but it is not
32-aligned, so it does not come from a 32-aligned calculator. The failure path still records
`caller=%p` in `exp_trace`. **Next step:** with 24 MiB more arena, watch whether the rate changes; if it
recurs, symbolise `caller=%p` with `arm-vita-eabi-addr2line -e build/tpv_vita`.

### 7.2 BMD loader invalid reads — ROOT-CAUSED AND FIXED (2026-11-02)

**The two recorded addresses were stale and one label was wrong. Do not reuse them.**

| Recorded | What the current ELF says |
|---|---|
| `makeHierarchy` +432 = `0x81ba8b48` | `makeHierarchy` starts at `0x81ba87d8` and is `0x1e6` bytes, so `+432` is **past its end**; it resolves to `J3DModelData::clear()` at `0x81ba8b40` |
| `loaderBasicBmd` +838 = `0x810e506e` | mis-added: `0x810e4cb0 + 838` = `0x810e4ff6`. `0x810e506e` is **inside a literal pool** — data, not an instruction |

**The real defect, found by disassembling rather than trusting labels.** In
`J3DJointTree::makeHierarchy()`, `J3DModelLoader` calls `makeHierarchy(NULL, &hierarchy)`, so **`pJoint`
is NULL on the top-level call**. The material and shape arms then read
`newMaterial != NULL && pJoint->getType() == 'NJNT'` and, in the shape arm, `pJoint->getMesh()` through a
shadowing local with no null check. `J3DJoint::getType()` returns the literal `'NJNT'` and never reads
`this`, so `pJoint->getType()` **succeeds through a NULL pointer** and the compiler may treat the branch
as taken. GCC did, folding the next statement into a read of absolute address `0x58`:

```
81ba895a: 2000   movs r0, #0
81ba895c: 6d80   ldr  r0, [r0, #88]   @ 0x58 == J3DJoint::mMesh
```

`0x58` is `J3DJoint::mMesh`. A `kTypeMaterial` or `kTypeShape` record at the **top level** therefore
guaranteed a null dereference; `kTypeJoint` at top level is safe.

**Fix:** guard both arms with `pJoint != NULL` (a top-level material/shape record is now skipped); remove
the shadowing `J3DMaterial* newMaterial` (renamed `pMesh`) and check `pMesh != NULL`, because `getMesh()`
can legitimately be NULL. Verified: the `movs r0,#0` / `ldr r0,[r0,#88]` pair is gone, every `mMesh`
access goes through the live joint register, and the shape arm has an explicit `cmp.w ip,#0` / `beq`.

**This is inherited, not a port regression**: the file is unmodified relative to dusklight `77705f8`, and
the reference decomp in `tp/` has the same code. **Its causal role is unproven.** DuskOnVita ships
byte-identical code and boots and plays, so on a well-formed disc the defect is latent. Correct wording:
*a genuine reachable null dereference on this path is fixed; the earlier log is still unexplained.*
Note also that `do_alloc` aborts on `TARGET_PC`, so if this were the same defect as §7.1 it would present
as `SIGABRT`, not a graceful failure.

### 7.3 Every texture upload was failing — ROOT-CAUSED AND FIXED (2026-11-02)

The largest rendering defect on the board, invisible until the frame was inspected rather than trusted.

**Symptom.** **31,786** `GL error uploading` warnings in one run — effectively every texture: `GX Static
Texture` (31,062), `GX Static Palette Texture` (710), `Resolved Texture` (13), `Copy Conv Texture` (1).
Frames still "rendered". Decoding the PNGs exposed it: red pinned at **255 in every row band**, **no
cool tones at all**, and only 3,093–3,979 distinct colours — the signature of untextured geometry.

**Why the log could not name it.** `had_gl_error()` drained the queue and kept a bool, discarding the
enum, and counted pre-existing errors from unrelated calls.

**Diagnosis, in order.** (1) Keeping the code showed **`GL_INVALID_ENUM`** for every size, mip count and
pixel/no-pixel case, which rules out size, NPOT, mip-chain and allocation causes at a stroke.
(2) `GL_RGBA`/`GL_RGBA`/`GL_UNSIGNED_BYTE`/`nullptr` are legal, and `GL_RGBA` is 0x1908 in both
`GLES2/gl2.h` and `vitaGL.h`. (3) A checkpoint after each GL call showed the error was already queued at
**`glPixelStorei`**, before `glTexImage2D` — which was never failing. (4) Reading vitaGL's implementation
out of the archive:

```sh
arm-vita-eabi-ar x $VITASDK/arm-vita-eabi/lib/libvitaGL.a textures.o
arm-vita-eabi-objdump -d textures.o | sed -n '/<glPixelStorei>:/,/^$/p'
```

```asm
cmp  r0, #0xCF2        ; GL_UNPACK_ROW_LENGTH, and nothing else
moveq ...; streq r1, [r3]
movne r3, =vgl_error
strne r2, [r3]         ; r2 = 0x500 = GL_INVALID_ENUM
```

**vitaGL implements `glPixelStorei` for `GL_UNPACK_ROW_LENGTH` only.** Every other `pname` sets a sticky
`vgl_error = GL_INVALID_ENUM`, so `glPixelStorei(GL_UNPACK_ALIGNMENT, 4)` was a no-op that poisoned the
queue, and `create_texture` discarded the texture it had just uploaded correctly.

**Fix** (`vita_texture.cpp`, overlay): `set_unpack_alignment()` skips the call on `__vita__` and keeps it
on the host — all uploads are RGBA8, so rows are always 4-byte aligned on Vita and the call is real on
Mesa. `report_gl_errors()` replaces `had_gl_error()`, naming each enum and logging each distinct
`(label, code, size, pixels)` **once**. `create_texture` no longer throws the texture away.

**Verified:** warnings **31,786 → 0**; distinct colours **3,979 → 78,360** and **3,093 → 87,870**;
bbox unchanged at `(271,128)–(1008,671)`; host texture suite still 3,780/0 with its negative control.

**Still true afterwards:** **0.0% of pixels have B > R**, i.e. no cool tones. Whether that is correct is
unknown — the opening area is legitimately warm, and it cannot be settled without a reference. Do not
read "textures upload" as "the scene is right".

**A latent instance remains** at `vita_gl_replay.cpp:1334` (`glPixelStorei(GL_PACK_ALIGNMENT, 1)` in
`read_efb_rgba`). Harmless only because `read_efb_rgba` has **no callers**; it becomes a bug the moment
anything calls it, and `1` was the wrong pack alignment for RGBA anyway.

### 7.3b Per-draw allocation in `bind_fog_range`, removed (2026-11-05)

`bind_fog_range()` runs for **every draw with fog-range adjustment** (~440/frame in the play scene) and
built a fresh `std::vector<uint8_t> rgba(width * 4)` per call — one allocation plus a fill per draw,
thrown away. It now rebuilds into a reused `sFogRangeScratch`, the pattern `sScratch` already used.
`resize()` does not shrink capacity, so steady state allocates nothing. The `sFogRangeRgba` comparison
that skips an identical upload is unchanged.

**No state-cache win was available**: `apply_pass_state`, `use_program`, `bind_texture` and
`enable_attribs` were already gated on `sCache`. The one call that *looks* wrong,
`set_enabled(GL_DEPTH_TEST, sCache.depthTest, true)`, is correct: the signature is
`set_enabled(cap, int8_t& shadow, bool enable)`, so the cache entry is the by-reference shadow. Check the
signature before "fixing" working code.

**Verified:** e2e 0 failed, texture 3,780/0, 0 `fog range LUT out of range` warnings, frame content
structurally identical. **The timing result is NOT a proven win** — frames 1200–1800 went 212.7 → 200.1 ms
and 2400–2760 went 218.6 → 162.0 ms, but §7.9 recorded 171–504 ms for the same configuration, so one
before/after pair cannot separate this from noise. The mechanism is real and free; **do not quote a
percentage**.

### 7.3c Draw batching: measured, and NOT worth doing — 442 draws only reach 377 (2026-11-05)

Draw-call reduction was the last candidate the emulator could resolve, because it cuts GPU-side work
rather than guest CPU work. **The answer is no**, and this section exists to stop a future session from
re-deriving the optimistic number.

```
batching survey @frame 960: 442 draws -> 17 pipelineGroups, 63 stateGroups, 211 adjacent runs
  (program+textures), 442 STRICT runs (offset+immediate); of 441 adjacent pairs: 0 share a uniform
  OFFSET, 69 share uniform BYTES, 65 are fully mergeable (same state+bytes);
  batched call count 377 (longest run 7)
```

| measure | count | meaning |
|---|---:|---|
| draws in the frame | 442 | |
| `pipelineGroups` | 17 | same `PipelineConfig` only. Optimistic ceiling, **not achievable** |
| `stateGroups` | 63 | same program **and** same texture bind group |
| adjacent runs (program+textures) | 211 | state-identical runs in submission order |
| `STRICT runs` (+ uniform offset + `immediateData`) | **442** | every draw is unique |
| adjacent pairs sharing a uniform **offset** | **0** of 441 | nothing is reused by address |
| adjacent pairs sharing uniform **bytes** | 69 of 441 | the only sound test |
| fully mergeable pairs | **65** | |
| **batched call count** | **377** | longest run 7 |

**442 → 377 is 1.17×, not 2.6×** — a 15% reduction bought by rewriting the decode stage to append into
one contiguous buffer with per-source index offsets, in a hot path Vita3K cannot show a win for. The
cost/benefit is not close: **closed, not deferred.**

**The same mistake was made twice, in the same direction.** The first survey counted only the pipeline
and reported "5.6×"; the second folded in the texture bind group and reported "2.6×". Both counted
state-identical draws, but a `glDraw*` also carries **one uniform set**, and `gx::DrawData` has per-draw
`uniformRange` plus `immediateData`. Both errors came from **stopping at the criterion that was
instrumented instead of writing the full legality condition down first**.

One sub-finding worth keeping: **`currentPnMtx` does not block a merge** — it is baked into each decoded
vertex (`vita_vertex.cpp:427` sets `p.constant`) and the matrices live in the shared uniform array.
Conversely, uniform **offset** is the wrong test: the frame arena reuses storage, so different offsets
routinely hold identical bytes (0 shared offsets, 69 shared blobs). Compare content, not addresses.

**Two instrument bugs produced empty or misleading logs.** The survey was gated on
`sFrameIndex == 2760` while the driving script ends near frame 2400, so it never fired — and a careless
edit had nested the timing window inside that gate, silencing every `window @frame` line too. An
instrument that can silently not run is worse than none: an empty log reads exactly like "nothing to
report". The next gate, `draws.size() >= 100`, fired at frame 240 — a **loading** frame with 163 draws.
Final form: sample up to 6 frames with `>= 350` draws, print one line per sample, and latch on a count
rather than a frame number. `kSurveyMinDraws = 350`, `kSurveyMaxSamples = 6`.

**Status of the code:** the survey is left enabled for now (six log lines per run, and the evidence for
this section). Delete it once this record is trusted — the GX replay path is the hot path.

### 7.4 Not started

- Real hardware testing (nothing has run outside Vita3K).
- Audio (`DUSK_AUDIO_DISABLED`; the audio engine is told it initialised, which is a lie to pass boot).
- ~~A reference render.~~ **De-scoped 2026-11-09**: the user validates the picture from run screenshots.
- Save **reload**, pause menu, and most of the game's content.

### 7.4b Upstream status and the fixes taken from DuskOnVita (2026-11-10)

**Upstreams are quiet. Verify before re-deriving:**

| Repo | Our pin | Upstream tip | New commits since our pin |
|---|---|---|---|
| `TwilitRealm/dusklight` | `77705f8` | `77705f8` | **0** |
| `encounter/aurora` | `3227d76` | `f77492f` | **24** |

Nothing to pull for dusklight. The 24 aurora commits cannot be taken: they are the `aurora::input`
migration plus `505b5eb`'s deletion of 22 public `PAD*` functions, which breaks dusklight's own sources
(see the header comment in `vita/aurora/patches.py`). DuskOnVita is on the **same** dusklight pin, so its
aurora work is directly comparable rather than ahead of us.

**Three correctness fixes taken, each verified against the source and against our own numbers:**

1. **GX min-filter table had two entries swapped.** `kHwToGxFilter` in `overlay/gfx/texture.hpp` inverts
   `GX2HWFiltConv = {0x00,0x04,0x01,0x05,0x02,0x06}`, so hardware bits `0x2` must decode to
   `GX_NEAR_MIP_LIN` (4) and `0x5` to `GX_LIN_MIP_NEAR` (3). Both were swapped, so a texture set to
   linear minification with nearest mip came back as point minification blending two mips, and
   `gl_min_filter` sent it to `GL_NEAREST_MIPMAP_LINEAR` instead of `GL_LINEAR_MIPMAP_NEAREST`.
   **Active in our build** — nearest sampling where the game asked for linear, i.e. shimmering in motion.
   Same defect as upstream `26371ce` (#293).
2. **Light directions were not normalised.** The generated fragment shader used the raw direction in the
   diffuse terms (`vita_shader.cpp:780` `dot(mv_nrm, ldirL)`, `:771` `dot(ldir, ldirL)`) while the emboss
   path in the same file does call `normalize`. Dusklight stores whatever the game passes to
   `GXInitLightDir`, which is not guaranteed unit length, so intensity was scaled by the vector's
   magnitude. **Active in our build.** Fixed by normalising each light in `gx/shader_info.cpp` before the
   bulk append (upstream `0b39c95` cannot be taken verbatim: at our pin the whole array is appended in
   one call, so there is no per-light site to wrap).
3. **The save path allocates 64 KiB on an 8 KiB stack.** `copy_stream()` in aurora's `io.cpp` has a
   `std::array<uint8_t, 64*1024>` local, and every card write goes through it (`open_atomic_file` →
   `copy_stream`, from `CardRawFile.cpp` and `FileIO.cpp`). The thread is the memory-card thread from
   `m_Do_MemCard.cpp`, whose `STACK_SIZE` is **`0x2000` (8 KiB)** — `0x1000` only under `PLATFORM_GCN` —
   and dusklight's `OSCreateThread` runs the body on a detached `std::thread`, so that is the real stack.
   The static `MemCardStack` array is bookkeeping. 64 KiB against 8 KiB is an **8× overflow on the first
   write**: on hardware this presents as "saving crashes", with no address clue pointing at a 64 KiB
   local. Fixed to 4 KiB chunks (the loop already handles short reads). **Vita3K cannot reproduce this** —
   host stacks are megabytes — so no emulator run would have shown it.

**One latent fix worth nothing yet:** negative LOD bias was cast float→`u8`, which ARM saturates to 0.
Patched in `patches.py`, but **`kUseTexLodBias = false`**, so nothing consumes it and no pixels change.
Recorded so nobody later credits it with a rendering improvement.

**Verified:** clean build, VPK produced, e2e 91 scenes / 14,266 checks / 0 failed, texture 3,780/0.
**Not verified:** no pixels were compared on a real scene, so none of the fixes is shown to improve the
picture — only that none can regress the suites.

**The decoding fixes now have tests, and getting that right took a correction.**
`tests/host/e2e/scenes_texobj.hpp` adds four scenes that drive the real `GXInitTexObjLOD`,
`GXInitTexObjFilter`, `GXInitTexObjLODBias` and `GXLoadTexObj` through the real FIFO and command
processor, then read back `gxState().loadedTextures`. Before, `grep -rn 'min_filter|lod_bias' tests/host/`
was empty, so both defects could have been reverted with every suite still green. Proven by reverting
each defect: the swapped table is caught in exactly the two slots it corrupts. **The LOD-bias UB is not
detectable at runtime on x86** — at runtime gcc emits `cvttss2si` and stores the low byte, so
`(uint8_t)(32.0f * -4.0f)` gives `0x80` in both buggy and fixed form; only at compile time does gcc fold
it to `0`. Same expression, two answers: the signature of UB, and the same divergence ARM's saturating
conversion produces. So the fix is pinned by `static_assert`s over a `constexpr` encoder, where the buggy
form is ill-formed (`overflow in constant expression`). Verified both ways. Fix 3 is not tested here — it
is a stack-depth constraint no host run can observe.

**Audited and deliberately NOT taken** (so this is not redone). DuskOnVita carries 17 aurora patches,
53 dusklight, 2 borealis and 5 vitaGL; most is performance work written against *their* GXM backend
(`command_processor.cpp` register-decode fast paths, static display lists, a GX worker thread, frame
overlap).

- **vitaGL patches (3 substantive):** we consume a prebuilt `libvitaGL.a`, not source. Adopting them
  means building vitaGL from source — a project, not a cherry-pick.
- **`borealis/0002` (absolute device paths):** real defect class, but it only affects `borealis::data`,
  which we do not use for game paths.
- **abseil patch:** byte-identical to our own `patches/abseil-vita.patch`.
- **`aurora/0012`:** the same min-filter fix already applied here; we also took the idea for the tests.
- **`aurora/0005`** is fix 3 above.
- **Upstream aurora's 24 commits:** only four were candidates, all no-ops for TP. `f77492f` (truncate
  vertex count, skip 0 draws) is ordering, not correctness — decode already clamps to
  `availBytes / vtxStride`, `draw_op` already returns early on `vtxCount == 0`, and GL ignores incomplete
  primitives. `6dc60dc` (`GXSetZScaleOffset`), `8aa4c09` (`GDSetTexImgPtr`) and `9873709`
  (`GXClear`/`GXClearSubRect`) add functions the game never calls. The rest are behind the `505b5eb` wall.

**A third port exists, runs in Vita3K, and rendered the frame this project de-scoped.** `capitanbarbacoa`
publishes "Dusklight PSVita" at <https://capitanbarbacoa.itch.io/duskgxm> (v1.1.0, 22 MB, built
2026-11-03, AI-assisted, code not shared); the workspace holds a copy at `../Dusklight-1-1-0.vpk`.

**It was run, not just read.** The same harness, the same `GZ2P01.iso` (symlinked to
`ux0:data/dusklight/Zelda.iso`, the path it expects). Result, saved as
`tests/vita3k/screenshots/theirs_prelaunch_{1..4}.png`: it reaches its prelaunch/preset screen **on top of
a fully rendered 3D scene** (the Ordon spring backdrop), consecutive frames differ by **30% of
scene-crop pixels** (max-abs-diff 149), and it does not crash. It crashed once on a fresh firmware-less
work dir — a setup fault: it wants `ur0:data/external/libshadccg.suprx`, and the emulator's own
`ur0:data/libshacccg.suprx` is enough for ours.

**Two earlier claims were wrong and are retracted.** (1) *"The `eboot.bin` is encrypted (`flags
0x100c0`), so it is not analyzable."* **False**: our own eboot carries the identical flags, which are not
an encryption marker. The real reason `strings` finds nothing is **compression** — valid zlib streams
start at `0x1000`, and a plaintext `\x7fELF` sits at `0xa0` in both. It **could** be unpacked (§ below).
(2) *"`res/rml/vita.rcss` does not apply because we stub RmlUi."* True today, but it understates what
their run proves: the Dusk UI works on this GPU in the emulator, so the gap is **ours**, not the
hardware's or aurora's.

**Their eboot WAS unpacked, and it says one thing that matters.** `tools/unpack_self.py` decompresses a
SELF in place, and it is **validated against our own build first** — the output must contain
`Aurora (vitaGL) initializing`, `ux0:data/tpvita/shader_fail` and `input script: ACTIVE`. It does, so the
unpacker is trustworthy before it is pointed at a foreign file. Two traps are documented in the script
because both cost time: `SceHeader` flags `0x100c0` do **not** mean encrypted, and the program headers are
**not** `Elf32_Phdr` even though `e_phentsize` is 32 (`p_offset`/`p_vaddr` are SCE segment descriptors, so
walking them yields plausible nonsense: `type=0x0159a284`). What works is scanning for zlib stream headers
and inflating each stream in place. Result: 31 MB of readable image for ours, 32 MB for theirs.

**Their build is stripped**: `aurora::gx::` appears 5 times in ours and 0 in theirs, `dusk::` 26 vs 1, and
the GXM/vitaGL entry points are imported by NID rather than named. **There is no symbol-level comparison
to be had**; do not repeat this expecting one. What *is* readable is structural and significant:

| | ours | theirs |
|---|---|---|
| WGSL source literals (`@fragment`/`@vertex`) | **0** | **5** |
| `textureSample` (WGSL builtin) | **0** | **15** |

**They ship upstream aurora's WebGPU shaders as WGSL and translate them to GXM at runtime; we generate
GLSL from GX state.** That explains a lot at once: why their patch set is careful edits inside aurora's
`lib/gx/` (they keep upstream's shader path), why their filter fixes matched upstream's (they track
upstream), and why our two fixes were needed — on our route the GX→GLSL generator is *our* code, so
upstream's shader fixes never reach us. It is also the honest answer to "should we switch routes?"
Theirs gets upstream fixes for free; ours avoids a WGSL→GXM translation layer on a Cortex-A9. Neither is
obviously right, and this is not evidence to switch.

**Their screenshots are NOT a valid pixel reference, and this is the trap.** The frames are live, but they
show a **different scene at a different time of day** (their prelaunch backdrop vs our in-game dialogue
and field). Comparing region luminance would be meaningless, and this project has published wrong
conclusions twice by comparing unlike things (§7.8). Using their build as a reference needs **both** ports
driven to the *same* scene, camera and frame — possible with their preset/Play UI, not done here. Until
then, treat their frames as proof that a correct picture is reachable on this GPU.

What it establishes:

- **A hardware-tuned performance ceiling for the same GPU.** Their notes: aiming for ≥25 FPS, ~80% of the
  game at 30 FPS, requires an **overclocked** console, and they recommend reducing resolution. Do not
  expect 30 FPS unclocked at native resolution from this port.
- **`res/rml/vita.rcss` is a hardware-measured list of what to remove from the Dusk UI**: every
  `backdrop-filter`, `box-shadow` and `filter:` becomes a flat background or a border, with numbers
  attached (a 22-mod catalog page took **8.5 s**; GPU frames of **300-600 ms** from `GeometryBoxShadow`
  and `SaveLayerAsTexture`, each shadow rendered into its own layer). Our build renders **no** RmlUi
  documents today (it compiles only `icon_provider.cpp` and stubs the rest), so nothing needs fixing yet —
  but this is the file to copy the day the Dusk UI is wired up here.
- **They ship a precompiled shader cache and we do not.** `shader_paketote.bin` (5.75 MB, magic `DSPK`,
  ~4334 entries) is a *cold* cache baked at build time. Ours is only *warm*:
  `vglSetShaderCachePath("ux0:data/tpvita/shader_cache")` persists what the console already translated,
  so the **first** run pays in full — §7.8 measured **200–700 ms per program** with the first ~70 built
  lazily, i.e. tens of seconds of stutter on a freshly launched game. Vita3K cannot show this, and
  **nobody has verified that our cache file is written on a real Vita**. When hardware arrives this is the
  first thing to measure.

**What DuskOnVita has that we should ask about, not re-derive.** Their console results are what our
Vita3K-only workflow cannot produce: GX worker time down 6.6 ms in Ordon village, 8.0 in Kakariko, 13.6 in
Hyrule Field (18 → 28 FPS); `C_MTXConcat` and J3D envelope sums through NEON, verified bit-exact over
**10.6 million** products, 1.5–2.3 ms off Castle Town's main thread. Every fast path ships with a
`--vita-fast-*=verify` mode comparing against the original path — the pattern §7.8 says we need. Their
`NathanKneT` offered a two-way exchange on issue #1: our synthetic GX scenes as a regression suite for
their GXM backend, their hardware bench and start-up/save-safety code for us.

### 7.4c vitaGL: the pin moved from r1448 to r1488 (2026-11-11)

The upgrade question is answered by the packaging, not the upstream tip. We consume a **prebuilt**
`libvitaGL.a`, so the only revision available without a source build is what the followed channel ships.

| | value |
|---|---|
| channel | `2026.08` (**stable**, not nightly) |
| before | `r1448.gcd3791e`, commit `cd3791e` (2026-08-30) |
| after | `r1488.g2bdbe89`, commit `2bdbe89` (2026-09-22) |
| channel sequence | `2026091245` → `2026111076` |
| also moved | `SceShaccCgExt` 1.0.1-1 → 1.0.1-2, plus 24 unrelated packages |

Upstream tip is 78 commits ahead (`e1473ffa`), so the channel is deliberately behind; the remaining ~38
need a source build. Commit ranges were read from the GitHub compare API, not inferred from dates.

**API compatibility was checked before rebuilding:** the `vitaGL.h` diff is **two added lines**
(`GL_DOT3_RGB`, `GL_DOT3_RGBA`) and **no removals**, so no call site could break.

**What the 40 commits cover.** Mostly the fixed-function pipeline (`GL_LIGHTING`, `glPopAttrib`,
display-list enqueueing, `GL_COLOR_MATERIAL`), which is dead code for us — `grep` over
`vita/aurora/overlay/gfx/` finds zero hits for `glBegin`, `glMatrixMode`, `glLight*`, `glGenLists`,
`GL_QUADS`. The commits that do touch our paths: `266f8a23` (`glShaderSource` limit 512, no double scan),
`cadd4600` (growable buffer in the GLSL translator), `f2959c1d` (`reserve_attrib_pool` wraparound),
`f7e1b73d`/`21abeda5` (safety guards), `7c9236bd` (circular pool sanity check).

**Verified after the upgrade, all re-run:** VPK builds and links, `check_vpk.py` 0 errors, host suites
**91 scenes / 14,266 checks / 0 failures**, and in Vita3K the same scripted run reaches the intro narration
and **in-game Ordon gameplay**. Frames kept as `tests/vita3k/screenshots/r1488_intro_narration.png` and
`r1488_gameplay_ordon.png`.

**One result could not be explained, and is recorded rather than buried.** A first post-upgrade run
produced **16 uniformly blank 263-byte frames, including the gameplay frames**, while the guest was alive
(`input_script.log` ran to frame 8990). Before blaming vitaGL: the installed app was **byte-identical to
the new VPK** (`cmp`), so it was the r1488 build (the `boot.log` timestamp is a red herring — the build
stamp comes from `__DATE__`). Two findings then ruled out a regression: `fatal.txt` and `crash.log` in
`ux0:data/tpvita/` are dated **2026-11-05**, stale files from an old session; and re-running the **same
script on the same binary** rendered gameplay correctly. So it is **not reproduced and not explained**.
Suspects, neither confirmed: stale `Xvfb :99`/window state, and the first frame being the normal blank
boot frame (a blank first frame is normal; a blank *sixteenth* is not). Treat "Vita3K renders" as a
**per-run check**, not an assumption from last session.

**Two operational notes that cost time:**

- `vdpm` is a subcommand wrapper: `vdpm refresh <channel>`, then `vdpm upgrade`. Pacman-style flags fail
  with `only one operation may be used at a time`.
- An unrelated upstream busy-spin can fill the disk: `dusk/mods/loader/loader.cpp:363` calls
  `fs::remove_all()` on `mods/.cache/dev.twilitrealm.luau`, and because that path is a **directory**, the
  POSIX `remove` behind it fails with `0x80010002` on every retry. One run wrote **6.2 GB** of
  `vita3k.log` (42.9 M lines); an earlier, pre-upgrade run had already written 919 MB, so it predates the
  upgrade. `rm -rf mods/.cache` neutralises it.

**To undo:** the old `libvitaGL.a` and `vitaGL.h` are saved with SHA-256 digests in
`psvita/tooling/backups/tpv-sdk-backup-vitagl-cd3791e/`.

### 7.5 The DuskOnVita audit: what another port already answered (2026-11-02)

[`NathanKneT/DuskOnVita`](https://github.com/NathanKneT/DuskOnVita) is an independent, MIT-licensed TP
Vita port — **not a fork of ours**, and materially further along. It is the reference implementation for
most of what is left on our board. Clone it to `/tmp/DuskOnVita`; nothing from it is vendored here.

**Console measurements (PS Vita 1000, 28 Oct 2026, build b2):**

| Scene | FPS | Bottleneck |
|---|---:|---|
| Ordon, outside Link's house | 27.9 / 27.4 | GX worker |
| Castle Town, twilight | 21.7 / 19.4 | main thread |
| Faron fight | 18.3 / 14.6 | main thread |
| Hyrule Field bridge | 17.6 / 19.2 | main thread and GX worker |
| Kakariko, twilight | 12.1 / 12.7 | main thread |
| Ordon village centre | 11.9 / 12.5 | main thread |

Its first build (22 Oct) did **6.3 FPS** — 4× in six days. It reaches the title screen, starts a new game,
plays the Ordon opening, saves, and runs at 444 MHz via `scePower` with no plugin, rendering at 75%
(720×408) scaled to 960×544, with ticks pinned to 30 Hz so speed holds down to 15 FPS.
**Conclusion: the port is feasible on real hardware.** That question is closed and never needed our VPK.

Where the two ports differ:

| | `tp-vita` | DuskOnVita |
|---|---|---|
| Integration | merged tree built at configure time; upstream untouched | submodule + `patches/` applied to a build *mirror* |
| Local delta | 14 dusklight edits, 1 aurora overlay | **17** aurora, **53** dusklight, 2 borealis, 5 vitaGL patches (an earlier reading of 11/34 was an undercount; §7.4b has the audited figures) |
| Renderer | vitaGL only | vitaGL **and** a hand-written SceGxm backend (`gfx_gxm/`), GXM default |
| Deps | `tools/fetch_deps.sh`, no hashes | `deps.lock` with SHA-256 per archive, digest-pinned SDK container, network off |
| vitaGL | SDK prebuilt, `r1488.g2bdbe89` (§7.4c) | built from pinned source `7635e3c` |
| Tests | host suites, no goldens | ~100 host tests plus `tests/fixtures/baseline/<sha>/` frozen goldens |
| Base | dusklight `77705f8` | dusklight `77705f8` (**same pin**; an earlier "3 commits older" note was wrong) |

**Independent confirmation of §7.3.** Its `gpu_api.cpp:431` comments *"vitaGL's unpack API supports row
length, not GL_UNPACK_ALIGNMENT. Every upload here is tightly packed RGBA8, already four-byte aligned."*
It never calls `glPixelStorei` at all. Two ports found the same trap by different routes, which is why it
belongs in the skill rather than being a local quirk. It also drains `glGetError()` once per frame and
keeps the enum (the rule in §8 trap 15).

**Its open bug is our open bug.** "Some textures flicker", and its help request is a *matched Dolphin
capture of the same scene* — the same missing artefact. It also notes it *"skips draws that would read an
empty copy rather than drawing them black"*, which says the saturation we saw was an **EFB-copy read
path**, not the texture path; both ports hit that wall and it chose to skip rather than sample.

**Taken from it:** the `#if !TARGET_VITA` guard on the retail arena trim (§7.1 — ours was live and removing
24 MiB per run, theirs was guarded from the start); `NOTICE` plus `LICENSES/` recording every pin and
licence, including the vitaGL LGPL position; the golden-fixture idea; and `--vita-exit-after` /
`sequence.txt` for self-terminating repeatable bench runs.

**Deliberately not taken:** its 53 dusklight patches. They are whole-game optimisation that assumes
behaviour may be rewritten per system, which contradicts the "behaviour comes from the decompilation"
posture this port holds. We cannot validate them, and importing them makes every future upstream diff
unreadable. Read its `docs/` as a specification for what to optimise, not as code to import.

### 7.6 First performance baseline, and why EFB copies are *not* the missing piece (2026-11-02)

**A `glReadPixels` probe of EFB copies was attempted and is VOID. Do not repeat it.** In this context
`glReadPixels` returned **all zeros even for the default framebuffer at frame 600**, when the captured PNG
proves the surface has content. A control read at fbo 0 was added specifically to catch this, and it is
what stopped "SRC all zero, DST all zero" from being reported as a broken EFB copy.

**What the counters did establish, and this is solid:** the resolve path is alive.

| Measure | Value |
|---|---|
| Resolve (EFB copy) ops, first 1000 frames | **11,338** |
| Per frame | **~11** |
| Copies that reached `draw_quad` | 11,338 / 11,338 (0 skipped, 0 with no destination) |
| Typical shapes | 608×448 → 304×224 (half) or 608×448 → 608×448, formats `0x13` and `0x6` |

So "our EFB copies are missing" is **wrong**. Whether their *contents* are correct is unknown and cannot
be settled by readback here.

**First timing baseline (Vita3K only — not a console frame rate).** One 1000-frame window: **151.94 ms**
per frame (6.6 fps equivalent), 174 draws, 71 shader programs, ~11 resolves. For scale, DuskOnVita measured
**154 ms/frame on real hardware** for its first build. Our emulator figure is the same order as their
*worst* console figure — the useful reading is that emulator timing here is dominated by software
rendering and shader compilation, so treat it as an upper bound on cost, not a prediction. **No FPS claim
about our port on hardware can be made from this.**

The `sResolveSeen` / `sResolveDrawn` / `sResolveNoDest` counters and the 300-frame window remain in
`vita_gl_replay.cpp`; they are cheap and produced the table. Remove them with the `mch_trace` bring-up
tracing once the record is trusted.

### 7.7 Render scale: 75%, measured (2026-11-02)

The EFB renders at 75% and `present()` upscales to 960×544. This is a **behaviour change** — the picture is
softer — and it is currently a `constexpr` in `ensure_efb()`; it should become a switch before anyone plays.

**A/B, Vita3K, identical runs, window ending at frame 1080 (71 programs, ~170 draws/frame):**

| EFB scale | Wall per frame | Equivalent | Distinct colours |
|---|---:|---:|---|
| 100% | 151.94 ms | 6.6 fps | 78,360–87,870 |
| 75% | ~113 ms | ~8.8 fps | 80,137–84,554 |

That is a **26% cut in wall time for a 44% cut in fragments**. The gap is the finding: frame cost is *not*
purely fill-bound — draw submission and ~11 resolves per frame are unchanged by resolution and dominate
proportionally more as the target shrinks. Do not expect a 44% win from resolution, and do not assume more
scaling keeps paying. Both figures are emulator figures; the ratio is the transferable part.

### 7.8 Three measurement traps that each produced a wrong conclusion (2026-11-02)

1. **A "hang" that was a SIGKILL during a buffered write.** Runs appeared to freeze while compiling
   `gx-1a49877e`, the log truncated mid-line. Wrong: the shader compiled in 283 ms in an earlier run.
   `in_container.sh` ends every run with `kill $pid; sleep 2; kill -9 $pid`, and a `Log.info` into a pipe
   is block-buffered, so the last line is cut mid-write. **A truncated log line means the process was
   killed, not that it stalled.** Use `Log.warn`/stderr for final state.
2. **A window that did not fit in the run.** A 300-frame window never fired in a 60 s run, because shader
   compilation costs 200–700 ms per program and the first ~70 are built lazily. Windows were cut to 120
   frames.
3. **Two blank-frame regressions that read as wins.** Forcing `separatePresentation` produced a rock-steady
   **33.33 ms/frame** — the simulation clock pacing an empty loop, because the interpolation presentation
   path does not draw on this backend. The screenshots were 263 bytes. **Check pixels before believing a
   frame time.**

### 7.9 Vertex decode cache: correct, 74% hit rate, and still not worth it here (2026-11-02)

DuskOnVita's profile puts vertex preparation first at **66 ms of a 154 ms frame**, so a decode cache was the
obvious thing to take. It is implemented in `vita_vertex.cpp` and **switched off** (`kVertexCacheEnabled = 0`).

**The first version was unsound and its 83.8% hit rate was meaningless.** Two defects, both found by reading
the decoder back: (1) **wrong buffer** — indexed attributes read from `frame.storage` (`Ctx::arr`), not
`frame.verts`, so the key hashed an overread past the vertex range and never included the array bytes the
decode consumes; two draws with identical index bytes over different arrays could **false-hit** and render
each other's positions, the worst failure mode a cache has. (2) **incomplete key** — decode also depends on
the `PipelineConfig` and on `draw.immediateData` (`currentPnMtx`, per-attribute `arrayStart`); neither was
hashed. The fix builds real `ArrayRange`s and folds the per-frame `frame.storage` digest, the config and the
immediate data into the key, with `memcmp` as the authority on the vertex bytes. Every hit rate taken before
this fix is void.

**Re-measured, sound, same scene (frames 1080–1200, ~165 draws/frame, 71 programs):**

| build | decode | hash + copy | total | per frame |
|---|---:|---:|---:|---:|
| cache OFF | 474.3 ms | — | 474.3 ms | **3.95 ms** |
| cache ON | 382.9 ms | 177.1 ms | 560.0 ms | **4.67 ms** |
| cache OFF (heavier window) | 773.4 ms | — | 773.4 ms | **6.45 ms** |
| cache ON (same window) | 344.3 ms | 235.8 ms | 580.1 ms | **4.83 ms** |

Hit rate **74–76%**, so decode work roughly halves. But the whole vertex path is only **4–6 ms of a ~130 ms
frame in Vita3K**, smaller than the noise, and on a faster host the hash+copy eats the win. Wall-clock A/B
was inconclusive by construction (171–504 ms for the same configuration); timing the cache internally is
what gives a low-noise number. Also: 20k–47k requests per window exceed `kVertexCacheSourceMax = 32768` and
are never cached, and evictions roughly equal misses — the working set exceeds the 12 MiB budget here.

**Why it stays off.** Vita3K runs guest ARM on a fast x86-64 host, so it **understates** CPU-bound guest work
by about an order of magnitude. DuskOnVita's 66 ms was measured on a **real Vita 1000**; if it transfers, this
cache is worth 15–45 ms/frame there and should be turned on. That is a hardware decision, which is why the
code is kept correct behind one constant. The general lesson: **in this emulator, measure the component, not
the frame.** A frame timer cannot resolve 1 ms when the noise is ±100 ms, and host/guest cost ratios do not
transfer.

**Correction: the intermittent core dump is a guest abort, not an unrelated fault.** Two runs died with
`Undefined instruction at 0x8227A58E` and a core dump, first written off here as "an unrelated guest fault,
reproducible with the cache compiled out". The reproducibility was true; the classification was not.
`0x8227A58E` is **inside our own image** (`.text` spans `0x81000040`..`0x822879D8`) and resolves to
**`_kill_r`**, newlib's syscall wrapper — the guest raising a signal against itself, i.e. `abort()` from an
assert or fatal path. `Trace/breakpoint trap` on the host is the emulator tearing down after that. It is
**not** the `0xBF18265F` failure: the crashed run's `dvd_trace.txt` ends with successful `JKRExpHeap::create`
calls and 7.5 MiB free. What is missing is the reason — `boot.log` was empty because the fatal path does not
flush. Do not classify a `Trace/breakpoint trap` as an emulator fault without resolving the address first;
one `addr2line` would have saved this correction.

### 7.10 The abort now names itself (2026-11-02)

Three things were wrong, each found by looking: **every reachable abort site was silent** (`PPCHalt`,
`OSSwitchStack`/`OSSwitchFiber` are a bare `abort()`, and `ConfigVarBase::markRegistered()` aborts on a
double register with no message); **no log had a critical line** (all 10 surviving logs returned 0 for
`|C||fatal|assert|abort|panic`); and **`boot.log` was empty** because the fatal path does not flush.

What was added:

- `vita/src/crash_record.cpp` — `__wrap_abort()` catches **every** `abort()` via `-Wl,--wrap=abort`, so
  dusklight needs no edits and the "behaviour comes from the decompilation" posture holds. It writes the
  context note, the caller address resolved to a symbol, and a SIGABRT dump, flushed line by line, to
  `ux0:data/tpvita/crash.log` (previous run kept as `crash.log.1`). The file is opened lazily, so a clean
  run leaves nothing.
- `vita/src/crash_symbols.cpp` + `tools/map_symbols.py` — a symbol table embedded in the image, so naming an
  address needs no filesystem access and no allocation on the fatal path. **Superseded by §7.11**: that
  table missed 43.8% of symbols and every size was 0. The table is now generated from the ELF with
  `nm -S -C`, ships as a file in the VPK, and `-Wl,-Map=` is gone.
- `tests/host/crash/` — 19 assertions against the real generated table.

**Two honest limits.** `assert()` is compiled out (`NDEBUG=1`, and the binary contains zero `assert`
references), so `__wrap___assert_func` is defensive only; enabling asserts in a release build of a game with
inherited asserts would trade one mystery for a new class of deaths. And `/proc/self/maps` is unavailable in
Vita3K, so the load base is not recorded — it did not matter because the emulator maps the ELF at its link
address.

**When it fires:** `crash.log` names the aborting function, which is the whole answer for the four silent
sites. `PPCHalt` or `OSSwitchFiber` means a stub this port has not implemented; `ConfigVarBase::registerConfigVar`
means a double registration, a real bug in the config system rather than a port defect. Fix the caller, not
the wrapper.

### 7.11 The recorder named the wrong function, and the fix (2026-11-03)

§7.10 shipped and then lied on its first real crash: `abort_caller=0x81c7f805 borealis::log::file_descriptor()`.
That function is a one-line atomic load at `0x81c7f3d8`, 0x10 bytes long; it cannot abort. `addr2line`
against the real ELF put the site in `borealis::log::detail::write_fatal()` (the `write_bootstrap_locked`
branch, immediately before `std::abort()`). So the finding was real and useful — the death is a
`LogLevel::Fatal` from borealis — but the name was wrong, and **a wrong name is worse than no name** because
it sends the reader to innocent code.

**The first diagnosis was also wrong**: "the table is one build stale by construction". Measurement killed
it — across two consecutive tables, **0 of 34,572 symbols changed address** and the ELF timestamp matched.

**The two real defects, both in the generator.** (1) **It threw away 43.8% of the table**:
`tools/map_symbols.py` rejected any name containing a space, and C++ demangled names contain spaces *and*
commas, so 31,439 of 71,787 parsed rows were dropped — `write_fatal` was not in the table at all, so the
nearest *surviving* symbol was reported. (2) **Every size was 0**: ld `-Map` symbol lines carry no size
column, so the "is this address inside this symbol" check could never fire — dead code that looked like
protection. `nm -S --defined-only -C` supplies both; it was available all along.

**Why the table is a file now.** A complete text table is **4.2 MiB** and `.text`+`.rodata` has only
**118 KiB** of slack before the data segment. The old table fit at 1.69 MiB only *because* it had discarded
43.8% of the symbols. It now ships as `app0:data/tpvita/crash_symbols.txt` (5.16 MB, 61,774 symbols), opened
only while dying, generated from the same ELF that ships — which removes the staleness class of bug. The
generator writes **real tabs and newlines**; the C-string-literal form silently produced a table the
resolver read as one malformed line.

**The check that makes a name trustworthy.** The table records the address of
`tpv_crash_table_anchor`; validity is that address matching the live one, otherwise the recorder prints
addresses only and says so. It ran on first boot and **failed on a correct table**: off by exactly 1, the
ARM **Thumb bit** (a function pointer has bit 0 set; the ELF symbol value has it clear). Both are normalised
before comparing. This is the argument for checking on every boot — a check you can only run when something
has already gone wrong cannot be validated.

**Verified end to end in Vita3K.** Every boot logs `crash symbols: table matches this build (anchor
0x8108f6f0); crash logs will name`, and a deliberate abort (a permanent opt-in trigger via
`ux0:data/tpvita/selftest-abort`, which deletes itself) produced `abort_caller=0x81009bab main`, where `main`
really does span `0x81008560`+`0x1814` = `0x81009d74`. The specific regression is pinned in the host test: an
address inside `write_fatal` resolves to `write_fatal`, and an address past the end of `file_descriptor()`
resolves to the *next* symbol. The tests read addresses out of the table rather than hardcoding them,
because hardcoded addresses test the build rather than the resolver.

**The lesson.** The recorder was built to remove an unknown and instead introduced a confident wrong answer,
which is strictly worse. Three things would each have caught it: assert on the artefact (43.8% of rows
dropped is visible in a row count), use a source that carries the field you are checking (sizes), and make
the tool state its own reliability. Only the third was in the original design, and a bug one layer down
defeated it.

---

## 8. Traps and gotchas

1. **`tp-vita/` is a git repo** (`Yannis100/tp-vita`), but `build/` and `deps/` are gitignored, so **a
   commit does not protect the tree you build from**. Before a sweeping change, snapshot the products:
   `tar -czf ../snapshot-$(date +%Y%m%d)/aurora_lib-merged.tar.gz -C build aurora_lib`. There is a precedent
   at `.snapshot-pre-aurora-upgrade-20261029/`. **Also:** the dusklight fixes (including §7.1 and §7.2) live
   in the separate `dusklight/` checkout and are **not** in the `tp-vita` remote — `git status` in
   `tp-vita` is clean while the game is patched.
2. **Never edit `build/aurora_lib` or upstream `extern/aurora/lib`.** Both are ineffective. Edit `overlay/`
   or `patches.py`, then re-configure.
3. **`patches.py` anchors must match exactly once.**
4. **A short Vita3K run gives blank screenshots.** Use `75 4`, not `40 2`.
5. **Delete `dvd_trace.txt` before each run.**
6. **On `TARGET_PC`, allocation failure = `abort()`.** `CRASH` → `OSPanic` → `Log.fatal` (`[[noreturn]]`).
   There is no soft failure path.
7. **Symbolise the fault PC before theorising.** `r2 == 6` at `_kill_r` means `abort()`, not a segfault.
8. **The IDE reports bogus C/C++ errors** in these files (`initArena2` undefined, `JKR_NEW_ARGS` undefined,
   `SDL3/*` missing, `GXRenderModeObj is not a type name`, …). These are clangd includePath artefacts.
   Confirm with a real build, do not chase them.
9. **`<aurora/gfx.hpp>` is unusable on the host** — it includes `<webgpu/webgpu_cpp.h>`. Same reason
   `patches.py` swaps out the WebGPU include.
10. **Do not hand-build link lines or add `-I` paths for the host tests.** Each has a `run.sh`.
11. **`dusklight/` modifications are uncommitted.** 14 files differ from `77705f8`; capture them with
    `git -C dusklight diff > /tmp/dusklight-local.patch` before any re-clone.
12. **Do not trust a recorded guest address without re-symbolising it.** §7.2 carried two stale addresses,
    one of them mis-added. Check the address lands inside the function's `nm` size and decodes as an
    instruction, not literal-pool data.
13. **`J3DJoint::getType()` returns the constant `'NJNT'` and never reads `this`.** `pJoint->getType() ==
    'NJNT'` is **not** a null check; the compiler may treat the branch as taken and fold the following
    member access into a read of a small absolute address. Test `pJoint != NULL`.
14. **vitaGL's `glPixelStorei` accepts `GL_UNPACK_ROW_LENGTH` only.** Any other `pname`, including
    `GL_UNPACK_ALIGNMENT` and `GL_PACK_ALIGNMENT`, sets a sticky `vgl_error = GL_INVALID_ENUM` and does
    nothing — and because it is sticky, a *later* `glGetError()` reports a failure that belongs to an
    earlier harmless call. Never set alignment on Vita. Verify from the archive, never the header:
    `arm-vita-eabi-ar x $VITASDK/arm-vita-eabi/lib/libvitaGL.a textures.o && arm-vita-eabi-objdump -d textures.o | sed -n '/<glPixelStorei>:/,/^$/p'`.
15. **A GL error boolean is worthless; keep the enum.** Log each distinct `(label, code, size)` once, or one
    broken path buries 30k identical warnings.
16. **GameCube *physical* addresses still compare against relocated Vita pointers, and the compares can
    pass.** `OSGetArenaHi() > 0x81800000` looked obviously dead and removed 24 MiB per run (§7.1). The same
    class hides in arena limits, MEM1/MEM2 ranges, `0x80000000`-based validation and `OSPhysicalToCached`.
    **Do not reason about these branches, measure them** — log both operands, then compare against the same
    run with the block compiled out.
17. **A defect being real is not evidence that it caused the symptom.** §7.2's null dereference is genuine
    and worth fixing, but another port ships identical source and boots, so it cannot be claimed as our crash
    cause. Before writing "root-caused", ask whether a second build that lacks the fix also lacks the
    symptom; if one exists, write "latent defect fixed, causality unproven".
18. **Validate an instrument against a known-good input before pointing it at a foreign file.** The
    "encrypted eboot" claim (§7.4b) cost two wrong conclusions; comparing against our own eboot was one
    command.

---

## 9. Files the port owns (`tp-vita/`)

```
tp-vita/
├── CMakeLists.txt              top-level build; text-gap check; VPK packaging
├── PLAN.md                     route to a playable port; milestones M1..M5
├── README.md                   build, install, run
├── HANDOVER.md                 this file
├── NOTICE                      every component, its exact pin and licence
├── LICENSES/                   the upstream licence texts NOTICE refers to
├── cmake/                      aurora.cmake, aurora_gx.cmake
├── vita/
│   ├── src/                    main.cpp, boot_diag.hpp, text_pad.cpp, input_script.cpp,
│   │                           crash_record.cpp, crash_symbols.cpp, dusk_ui_*_stub.cpp
│   └── aurora/
│       ├── backend.cpp         Aurora backend wiring (path ownership, per-frame input)
│       ├── patches.py          exact-once patches onto upstream aurora/lib
│       └── overlay/
│           ├── gx/             texture.cpp, vita_shader.cpp, vita_vertex.cpp, …
│           └── gfx/            vita_gl_replay.cpp, vita_frame.{hpp,cpp}, vita_texture.cpp, …
├── tools/                      build_aurora_tree.py, check_text_gap.py, map_symbols.py,
│                               unpack_self.py, fetch_deps.sh, build.sh
└── tests/
    ├── host/{vertex,shader,texture,disc,crash,e2e}/   each with run.sh
    └── vita3k/                 headless runner (run.sh, README.md) and screenshots/
```

The Vita-specific rendering work lives in `vita/aurora/overlay/gfx/vita_gl_replay.cpp` (GLES2 replay of the
recorded frame) and `vita/aurora/overlay/gx/vita_shader.cpp` (GX TEV/lighting → GLSL ES 1.00).

---

## 10. Suggested reading order for a new session

1. This file.
2. `PLAN.md` — milestones and the honesty section.
3. `README.md` — build/run, and what a pass proves.
4. `tests/vita3k/README.md` — what an emulator run proves and does not.
5. `vita/aurora/patches.py` — read the header comment; it explains why `505b5eb` must not be adopted.
6. `vita/aurora/overlay/gfx/vita_gl_replay.cpp` — the renderer.

---

## 11. If you have to choose one thing to do next

**Produce runs whose screenshots can be judged, and stop optimising blind.** The desktop reference render is
de-scoped (§7.4); the user validates the picture from run screenshots. That changes the deliverable: drive
far enough that the screenshots show gameplay, save them durably with the frame they came from, and **do not
describe a screenshot as "the scene looks right" unless it was compared to something**.

**Do not spend more time on frame-rate micro-optimisation in Vita3K, and draw batching is closed.** §7.8 and
§7.9 end with the same answer: this emulator cannot resolve a 1–2 ms change because its noise is ±100 ms, and
it runs guest ARM about an order of magnitude faster than a Cortex-A9, so CPU gains do not transfer. The vertex
cache is the worked example: correct, 74% hit rate, halves decode work, still not measurable. Batching was the
last candidate that could show up here, and it is closed on evidence at 1.17× (§7.3c). If a future session
wants a rendering win, the honest position is that none is left that this tool can prove.

**Two candidates that are worth real work, in order:** (1) **hardware**, because everything about save reload,
shader-cache write, performance and audio is unmeasurable here; (2) driving **both** ports to the same scene
and frame, which is the only way the third port's build becomes a valid pixel reference and the only way to
tell whether the §7.4b fixes improved the picture.

**If a run dies:** §7.1's arithmetic is now sane (24 MiB more arena), so watch whether the `0xBF18265F` rate
changes before doing anything else; the failure path already records `caller=%p`.

**The honest position as of this revision:** the port boots, reaches the main loop, renders correctly shaped
and correctly coloured frames, and reaches gameplay in an emulator. A separate port does the same on real
hardware at 12–28 FPS with working saves (§7.5), so feasibility is settled. Still unproven here: save reload,
performance, audio, and all hardware behaviour.
