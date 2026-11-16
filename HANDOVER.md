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

**Amended 2026-11-11 (§3d.7).** The GCI left on disk by that run has an **empty** `mPlayerName`, because
`initdata_to_card()` clears it by design. So the second run could not have shown a continuing game from
that file: an unnamed slot is what the file select draws as "New Quest Log". The "began a new game"
observation therefore carries no information about the load path, and §3d.7 explains how the file select
actually labels a slot. Reload stays untested until a save written after both name screens is loaded.

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

### 3d.7 Cross-build save test: the fixture is a valid, EMPTY save (2026-11-11)

The test being set up was "drive capitanbarbacoa's Vita build to the save our port wrote". Its premise
was wrong, and reading the bytes is what showed it. Recorded here so the next session does not run the
same experiment expecting a player name on the file select.

**What the run established, in order.**

1. **The card is mounted, with our file.** The guest log contains
   `[INFO | aurora::card] Loaded GC Card Image: ux0:/data/TwilitRealm/Dusklight/EUR/Card A`, and the
   emulator log shows the guest opening
   `ux0:/data/TwilitRealm/Dusklight/EUR/Card A/01-GZ2P-gczelda2.gci` twice. No
   `MemoryCardA.EUR.raw` was created. The card-type fix worked.
2. **The slot label is not a failure signal.** All three slots read `New Quest Log`. Before the
   screenshot was read, "New Quest Log" was taken to mean the save was not found. It means the
   opposite, and the source says so — [`dFile_info_c::setSaveData()`](../dusklight/src/d/d_file_sel_info.cpp:121):

   | condition | label | return |
   |---|---|---|
   | checksum invalid | `This Quest Log is Corrupted` | `-1` |
   | checksum valid, `mPlayerName[0] == 0` | `New Quest Log` | `1` |
   | checksum valid, name set | the player's name | `0` |

   `New Quest Log` therefore proves the checksums passed **and** the name field is empty.
3. **An empty save is what the game writes at that point.** [`initdata_to_card()`](../dusklight/src/d/d_save.cpp:1918)
   calls `save.init()` and then, explicitly and by design, `setPlayerName("")` and
   `setHorseName("")`. The GCI was written by a run that answered "Yes" to creating the save file and
   never completed a name entry, so an unnamed slot is the correct content.
4. **The file is structurally valid, measured.** `tools/gci.py` reports `exit=0`:

   ```
   header: GZ2P-01 name='gczelda2' blocks=4
   size:   32832 bytes, expected 32832 (ok)
   status: banner=1 anim=4 iconFormat=0155 iconSpeed=0155 iconAddr=0x0 commentAddr=0x2400
   primary @0x4000: dataVersion le=6 be=100663296 (ok)
     sector checksum: ok (big-word/little-store)
     slot 1: checksum valid  name='' horse='' time=0 stage='F_SP108'  displays='New Quest Log'
     slot 2: checksum valid  name='' horse='' time=0 stage='F_SP108'  displays='New Quest Log'
     slot 3: checksum valid  name='' horse='' time=0 stage='F_SP108'  displays='New Quest Log'
   ```

   (One `primary` sector is quoted; the `backup` at `0x6000` prints the identical six lines, and both
   are in the file. `dataVersion be=100663296` is the same four bytes read the other way — `0x06000000`
   — and is not a defect: `mDoMemCdRWm_Restore()` copies the int without swapping it.)

   `banner=1`/`anim=4` are `CARD_STAT_BANNER_C8`/`CARD_STAT_ANIM_BOUNCE`; `iconFormat=iconSpeed=0x0155`
   is five C8 icons then three `NONE`/`END`, which is exactly what
   [`mDoMemCdRWm_CheckCardStat()`](../dusklight/src/m_Do/m_Do_MemCardRWmng.cpp:436) requires. So the
   `Restore()` path is not rejecting anything.

**Two endianness facts, both measured, both easy to get wrong.**

- `mDoMemCdRWm_CalcCheckSum()` declares `u16 high, low`, so each accumulator wraps at 65536 before the
  `high << 16 | low` pack. Widening them to `u32` changes the result and reports a valid file as broken.
- `BE(u16)` is a no-op byte type on a little-endian host ([`endian.h:352`](../dusklight/include/helpers/endian.h:352)),
  so the sector checksum reads **big-endian words** but stores the `u32` result in **host byte order**.
  The file matches `big-word/little-store` and nothing else. Conflating those two axes produced two
  false "MISMATCH" reports before the pair was separated.

**What this changes about §3d.2's open question.** §3d.2 says a second run "began a *new* game rather
than continuing" and calls save RELOAD untested. Loading this GCI cannot demonstrate a reload: the slot
it produces is indistinguishable from an empty one, because it *is* an empty save. A reload test needs a
save with a non-empty `mPlayerName` — i.e. one written after both name screens complete. Until then
"save reload" stays untested, and a file-select screenshot showing `New Quest Log` is not evidence
either way.

**The fixture to build next.** Complete the player- and horse-name screens, then copy the resulting GCI
out and check it with `tools/gci.py`; a slot that reports `name='Link'` is the precondition for the
cross-build test to mean anything.

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
| printf | `./tests/host/check_printf_formats.sh` | clean — no `%z`/`%j`/`%t`/`%hh` in **code** under `vita/` (comments excluded; see §6.1) |
| e2e | `./tests/host/e2e/run.sh` | **91 scenes, 14,266 pixel checks, 0 failed**; `TPV_E2E_ASAN=1` for AddressSanitizer |

The shader suite **reports** configs exceeding Vita uniform limits; that is its purpose, not a failure.
Current maxima: `fsVec4=26`, `vsVec4=153`, `maxAttribs=19`; over the caps: 93 configs with fs>16,
104 with vs>128, 4 with attribs>16.

Do not hand-build a link line for a suite: each `run.sh` already does it with its own stubs. That
detour was wasted once already.

### 6.1 The printf guard was failing on its own documentation (2026-11-12)

`check_printf_formats.sh` grepped `vita/` for `%[-+ #0-9.*]*(z|j|t|hh)[diouxX]` and **matched a comment**
in `vita/src/input_script.cpp:351` that documents the `%zu` lesson — the note explaining why the guard
exists. So the suite was red while §6 called it clean, and the failure was indistinguishable from a real
violation in a summary line.

Fixed by stripping comments before matching: line comments, trailing comments and multi-line block
comments. String literals are deliberately **not** stripped, because a `"%zu"` inside a string is exactly
what the guard is for.

**Both directions proven.**
- Real violations still caught, including one on the same line as a preceding block comment:
  `int x = 1; /* "%zu is fine here" */ log.line("%jd", v);` → exit 1.
- Comments and strings that merely mention `%zu` / `%td` → exit 0.
- The real tree: exit 0, with no comment-only noise.

The rule this is an instance of: **a check that cannot pass is a check that will be deleted.** Before
trusting a red or green suite line, run it and look at what it actually matched — §6's "clean" was a
claim, not an observation.

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

**Update 2026-11-12 (§7.12b) — it recurs, and now reproducibly.** Two consecutive idle runs on
`build/tpv_vita-release.vpk` (dated 2026-11-11 11:56) aborted with
`JKRExpHeap.cpp:281 Aborting due to allocation failure!`, `crash.log` naming
`borealis::log::detail::write_fatal`. The guest log breaks off mid-line while the attract loop streams
`demo38_wl_cut01_gp_1_o.bck` / `_l.bck` / `demo38_wl_cut00_gp_1_o.bmd`, so the failing allocation is in
that resource load, not in `mDoMch_Create`. **2 of 2 is not a rate**, but it contradicts "one run in
seven … retired", so treat the failure as live. The specific `0xBF18265F` value was **not** observed in
these two runs — `JKRExpHeap.cpp:281` does not log the size — so the corruption arithmetic above is
still untested against these cases. **The next step is the same one §7.1 already named**, and it is now
cheaper: make `JKRExpHeap.cpp:281` print the requested size, alignment, heap name and free bytes, then
read them from `fatal.txt`.

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

**Their screenshots are NOT yet a valid pixel reference, and this is the trap.** The first frames we took
weren't a valid reference either: they showed **a different scene at a different time of day** (their
prelaunch backdrop vs our in-game dialogue and field). Comparing region luminance would be meaningless,
and this project has published wrong conclusions twice by comparing unlike things (§7.8). Using their
build as a reference needs **both** ports driven to the *same* scene, camera and frame. Until then, treat
their frames as proof that a correct picture is reachable on this GPU.

#### 7.4b.1 Their UI confirms on Circle, not Cross — and the config is the other lever (2026-11-11)

**The modal is clickable. The prior session pressed the wrong button.** Cross (`x`) never confirmed it —
measured twice, with a 0.12 s tap and a 1 s hold, four later screenshots pixel-identical — but that is
not a Vita3K defect and not a timing problem: **this UI confirms on the pad's `A` button, which on Vita
is Circle (`c`)**. The mechanism, read from source:

- `dusklight/src/dusk/ui/input.cpp:156-178` `map_pad_button()`: `PAD_BUTTON_A -> KI_RETURN`,
  `PAD_BUTTON_B -> KI_ESCAPE`.
- `dusklight/src/dusk/ui/ui.cpp:633-659` `map_nav_event()`: `KI_RETURN -> NavCommand::Confirm`,
  `KI_ESCAPE -> NavCommand::Cancel`.
- So the *east* face button (Cross) is `B` = Cancel, and the *south* button (Circle) is `A` = Confirm.
  The preset window prints "A/B/X/Y" and its Play/Settings/Mods/Quit column behaves the same way.
- Navigation always worked because the D-pad maps to `KI_LEFT/RIGHT`, which is why `hold Left` moved the
  selection and made the earlier session suspect timing.

**Proven by run** (`cc_` prefix, 2026-11-11): `hold Left 1` → Classic highlighted gold border;
`hold c 1` → the modal closes and PLAY takes the focus highlight, over the live disc scene. A previous
attempt with `hold Return 1` did **not** confirm either — Vita3K translates a host Return into the pad's
Start, not into a `KI_RETURN` keydown, so the host key that works is `c`.

**Why this matters beyond the modal:** loading an existing save from their in-game menu needs a working
Confirm, and that is the gate on the like-for-like comparison in §7.4b.2.

**The other lever is `config.json`, and it is exact rather than a guess.** Their build serialises every
registered `ConfigVar` to a flat JSON object (`dusk/config.cpp:538-567`), and the preset modal's Confirm
handler is `PresetWindow` (`dusk/ui/preset.cpp:102-112`): it calls `applyPresetClassic()` and then
`backend.wasPresetChosen = true; config::save();`. Writing those same keys therefore produces the same
state as clicking the button:

- `tests/vita3k/scripts/write_theirs_classic_config.py` writes a transcription of `applyPresetClassic()`
  (`preset.cpp:13-26`: `bloomMode`/`depthOfFieldMode` = `Classic` = 1, `internalResolutionScale` and
  `shadowResolutionMultiplier` = 1, `menuScalingMode` = `GameCube` = 0, toasts off, `enableMenuPointer`
  false, `lockAspectRatio` true), plus `backend.wasPresetChosen: true` and optionally
  `backend.skipPreLaunchUI: true`.
- `AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT)` is the one line of `applyPresetClassic()` that is not a
  config var. It requests the viewport our port already uses, which is consistent with the measurement
  below.
- Their config lived at `ux0:data/TwilitRealm/Dusklight/config.json`, i.e. the **same data directory our
  port uses**, and it already pointed `backend.isoPath` at `ux0:data/tpvita/GZ2P01.iso`. Their card
  setting `backend.cardFileType: 0` is `CARD_GCIFOLDER`, so both builds read the same
  `EUR/Card A/01-GZ2P-gczelda2.gci`.

**With that config their build boots straight into gameplay.** `d1_` run, three captures, no key presses:

| shot | distinct colours | bbox | mean | diff vs previous |
|---|---|---|---|---|
| `d1_1_01_early` | 46,660 | `(270,128)-(1007,671)` | 24.4 | — |
| `d1_2_02_mid` | 68,704 | `(270,128)-(1007,671)` | 43.9 | 39.1% of pixels |
| `d1_3_03_late` | 44,915 | `(270,128)-(1007,671)` | 42.8 | 37.3% of pixels |

The frames show the Hyrule Castle courtyard/gate cutscene with Epona and the sunset castle — live 3D, not a
menu. **Their bbox is `(270,128)-(1007,671)`, exactly one pixel left of ours `(271,128)-(1008,671)`.** The
width matches (738 columns either way); only the x origin differs. That is a measured agreement between
the two independent viewport implementations and worth knowing before any pixel diff: a naive
pixel-to-pixel comparison would be off by one column.

**A harness bug hid this for one run, and it is fixed.** `in_container_script.sh` called
`import ... 2>/dev/null || true`, so a failed capture left no file, no message and no non-zero status. The
first `s1_` run produced **one** file out of eight shots and reported success. The capture now prints a
warning with `import`'s own error and flags empty files. Anything that reasons from a screenshot count must
still check the file exists — see §7.8.

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

### 7.12 Three-port frame comparison: the pixel diff is not yet possible, and the number that would be quoted is meaningless (2026-11-12)

**Status (amended by §7.12c, 2026-11-12): a same-scene pair WAS produced — the file select, reached by
our build and measured against `sf_5_05_file_select.png` — and the two layouts agree to about one pixel.
Our half survives in `tests/vita3k/screenshots/ours_file_select_20261112.png`; the third port's half was
lost with its work directory, and three attempts to re-capture it did not reproduce the screen, so the
pair is **not currently re-measurable**. Relative *rendering quality* is still not established, and could
not be established from that pair even when both halves existed: the captures differ by an animated
cursor and by the pad's glyphs, which is by design. Everything below about why the earlier attempt failed
is kept because the failure mode is instructive.**

Three TP→Vita ports are relevant:

| port | code | runnable here | frames available |
|---|---|---|---|
| ours (`tp-vita`) | yes | yes (`build/tpv_vita-release.vpk`) | yes, new ones captured today |
| capitanbarbacoa "Dusklight PSVita" | **no** (itch.io, closed) | yes (`Dusklight-1-1-0.vpk`) | yes (`nm_`, `sf_`) |
| DuskOnVita (`NathanKneT`) | yes | **no** — GitHub publishes **no release and no VPK**; needs a full VitaSDK source build | **none on this machine** |

So a three-way image comparison cannot be produced today. Two of three can be run; the third cannot be
run at all without building it.

**The measurement that matters, and the trap it avoids.** Diffing frames from different scenes gives a
number that looks like evidence and is not. Measured, on the 738x544 content region:

```
ours(r1488_gameplay_ordon) vs theirs(nm_12_gameplay_a) : 90.9% of pixels differ
theirs(nm_12_gameplay_a)   vs theirs(nm_14_gameplay_c) : 21.0% of pixels differ   <- same run
```

90% is the scene change, not the renderer. A fingerprint correlation over all available captures
confirms it: **every cross-port pair is below 0.5**, while known same-scene pairs score 0.88–1.00.
There is no ours↔theirs pair showing the same screen anywhere in the archive.

**Why the screens do not line up, concretely.** The two builds' scripts produce different screens and
their shot labels are unreliable:

- `nm_4_04_title.png` is **not** a title screen — it is the file select. `nm_3_03_after_play.png` is
  the file select too. Reading a filename as evidence is the §7.8 trap again.
- Our idle run **did** reach the true title once (`PRESS START`, `©2006 Nintendo`, logo uncropped over
  the attract backdrop). That frame was not kept and **the capture is no longer reproducible** — see the
  crash note below. Their archive has no title capture to compare it to either.
- Pressing START in **our** build via the host harness does not advance the title (`Return` and `c`
  both left the frame unchanged across 10–12 s waits), so the file select is not reachable that way.
  The frame-counted injector does work (`input_script.log` ran to frame 2,860) but its save-reload
  script lands in gameplay, not on the file select.

**Geometry is NOT the blocker, and that was checked.** Both ports letterbox to the **same 738 px
width** at the same x on the 960x544 surface, so a same-scene diff would be legitimate. DuskOnVita is
the one that differs — its own test pins `viewport(118,0,724,544)` against our `(111,0,738,544)` — but
its frames cannot be captured here anyway. `tools/compare_port_frames.py` handles this by comparing only
the overlap and reporting the geometry separately, so a viewport difference is never smeared into an
image-error percentage.

**Tooling added so the next attempt does not redo this.**

- `tools/compare_port_frames.py` refuses a pair whose fingerprint correlation is below `--min-correlation`
  (default 0.85) and prints no diff at all. A refusal is the correct result for a different-scene pair.
  Controls verified: different scenes → refused (exit 1); same scene → measured (exit 0, 30.7%);
  identical file → 0.0%; a real same-screen UI pair (`sf_5` vs `nm_4_04`) → measured (r=0.977, 8.8%,
  the residual being the animated slot highlight). `--port-status` prints the table above so a report
  never implies a three-way comparison that did not happen.
- Our captures from today, all `738x544` and healthy (8k–85k colours):
  `our_1..6` (idle/attract, title at `our_5`), and the injector-driven gameplay frames.

**The one experiment that would settle relative rendering**, in order of cost: (1) drive **our** build
to the file select — the static UI both runnable ports provably render — and diff it against
`sf_5_05_file_select.png`; the injector must be given a script that reaches the file select instead of
loading a save. (2) Build DuskOnVita from source to obtain its frames. Until one of those exists, no
claim about which port renders better is supportable.

**Two separate startup faults found while doing this. Both are ours, not the renderer's.**

**(a) A `remove_all` that never returns.** A first idle run never got past the vitaGL splash in 90 s.
Cause: `ux0:data/TwilitRealm/Dusklight/mods/.cache/dev.twilitrealm.luau` existed as a **directory**, and
the emulator log showed an unbroken `remove_file ... Error code: 39 (Directory not empty)` loop.
[`loader.cpp:363`](../dusklight/src/dusk/mods/loader/loader.cpp:363) calls
`fs::remove_all(m_cacheDir, ec)` **once**, so `remove_all` itself is looping on Vita/Vita3K instead of
returning the error in `ec`. Removing that directory let the app boot and render normally. Not diagnosed
further, and **not** the blank-frame symptom: only short runs (≤90 s) then produced the documented
263-byte blanks.

**(b) A reproducible `JKRExpHeap` allocation abort at the attract loop — new evidence for §7.1.**
Two consecutive idle runs aborted with `Trace/breakpoint trap (core dumped)`, and the recorders named it:

```
fatal.txt : [/home/yanserm1/psvita/dusklight/libs/JSystem/src/JKernel/JKRExpHeap.cpp:281]
            Aborting due to allocation failure!
crash.log : abort_caller=0x81c81b0d  borealis::log::detail::write_fatal(...)
```

`fatal.txt` and `crash.log` are both fresh, and `symbols: table matches this build`. The guest log stops
mid-write — the last line is a truncated `Loading Resource: demo38_wl_cut00_gp_1_o.bmd (` — so the
allocation failure happens while the attract loop streams demo resources
(`demo38_wl_cut01_gp_1_o.bck`, `_l.bck`, then that `.bmd`).

What this changes about §7.1: the `0xBF18265F` corruption was recorded there as "the allocation is still
unexplained" and the rate as "one run in seven … 14 consecutive release runs produced no fatal". Here it
is **2 out of 2** on the current release VPK, at the title/attract stage. That is not a rate estimate
(n=2), but it is the first time this failure has been **reproduced on demand with a named caller** rather
than inferred, and it contradicts "retired". The earlier "14 clean runs" were idle runs too, so the
difference is likely tree state rather than scenario — `build/tpv_vita-release.vpk` is dated 2026-11-11
11:56, newer than those runs. Next step is §7.1's own: symbolise the allocation site and print the
failing size and heap figures, which `JKRExpHeap.cpp:281` does not currently log.

**One earlier capture of the title does exist from before these crashes** and shows the logo, `PRESS
START` and `©2006 Nintendo` over the attract backdrop at `738x544`. It was transient and is not in the
archive; re-capturing it is blocked until (b) is understood. Do not claim a title screen from this build
until then.

### 7.12c The file select WAS reached, and it is a valid same-screen pair with the third port (2026-11-12)

**Status: §7.12's central blocker is closed.** Both runnable ports have now been photographed on the
same screen — the file select — and the pair is geometrically the same screen, verified by
measurement, not by filenames.

**How the screen was finally reached.** §7.12 recorded it as "transient, missed by a coarse sampler".
Both halves of that were true and the fix was two changes, not one:

1. **The sampler was photographing a hole.** Run `fs_` had 19 screenshots and still missed the screen.
   Its mtimes say why: the last live-3D frame was at 16 s, the first menu frame at 25 s, and the
   transition sat in that 9-second gap with no screenshot in it. A coarse sampler does not merely miss
   a screen, it reports "the game never showed it". `waits_file_select.txt` now samples every 3 s
   through the whole boot -> title -> file select window.
2. **The press count was wrong in both directions.** A SINGLE START at frame 300 did nothing at all
   (attempt 2: 21 shots, every one the live attract loop, injector log confirming the step was
   applied). The reason is visible in attempt 2's samples: frames at 20 s, 23 s and 26 s were **blank**
   (263-byte, `mode=1`). Frame 300 lands inside the blank boot -> title transition and is discarded.
   Three presses starting after that window (frames 390, 570, 750) do get through — that is what
   attempt 1's three presses did — and the run then landed on the **file select** at `fs3_16`.

   **The lesson:** "press N times" and "press once" are both guesses about a state machine; only the
   blank-frame timestamps say when the title actually exists. Keep the blank frames in the record,
   they are the timing evidence.

**The captured screen.** `tests/vita3k/screenshots/ours_file_select_20261112.png`
(from `fs3_16_p_74s`): "Choose a Quest Log", three slots all reading `New Quest Log`, the selection
frame on slot 1, and the `B` / `Confirm A` bar — the GameCube prompt glyphs, on the parchment board.

> **The reference frame has since been lost, and re-capturing it did NOT succeed (2026-11-12).** Our
> capture survives (it was copied into `tests/vita3k/screenshots/`), but `sf_5_05_file_select.png` lived
> only in `/home/yanserm1/psvita/tooling/vita3k-work/out/`, a transient emulator work directory that was
> reset by a later run. Three re-capture attempts were then made and **none reproduced the screen**:
>
> | attempt | their config | result |
> |---|---|---|
> | `th_` | `skipPreLaunchUI:true`, `cardFileType:1` | 8 shots, all live 3D — no menu |
> | `th2_` | same, `cardFileType:0` (GCIFOLDER) | 8 shots, all live 3D — no menu |
> | `th3_` | 11-Nov 89-byte config (no `wasPresetChosen`) + `hold c` | reached the **prelaunch PLAY screen**, then 736x544 UI frames — not the file select |
> | `th4_` | `skipPreLaunchUI:true` + `cardFileType:0` + `hold Return` (Start) | 8 shots, all live 3D (60-74% deltas) — no menu |
> | `th5_` | `skipPreLaunchUI` **removed** (launch UI left up), `cardFileType:0`, `hold Return` | reaches their **launch UI at 736x544** and sits there (deltas 1.9-5.9%); not the slot list |
>
> `th5_` is the only attempt that changed the variable the others held constant, and it did move the
> picture: with `skipPreLaunchUI` removed the build stops in a static 736x544 launch screen instead of
> running 3D. That screen is **not** "Choose a Quest Log" — it has no three-row slot list and no
> parchment board (captured as `tests/vita3k/screenshots/theirs_launchui_20261112.png`). So the launch
> UI is on the path, but `hold Return` there did not produce the slot list either.
>
> **The cause of the first, second and fourth attempts is NOT established, and one wrong explanation was
> written here and then retracted within the same session.** It said `m_Do_main.cpp:914` gates the
> prelaunch flow behind `if (!dvd_opened)` and that this harness passes `--dvd`. That is false:
> `in_container_script.sh:32` launches `./Vita3K -c ... --frontend sdl -w /vpk/app.vpk` and passes
> **no `--dvd`**, so `dvd_opened` stays false (it is only set inside the `parsed_arg_options.count("dvd")`
> branch at `m_Do_main.cpp:811`) and the block at :914 **does** run. A source claim that fits the symptom
> is not a verified cause — the flag had to be grepped for in the launcher, and it was not.
>
> What IS verified about those runs: they render live 3D continuously, i.e. their build is past the
> menus and in the game. The plausible reading — not tested — is that with a save on the card and
> `skipPreLaunchUI`, their build continues into the game without ever offering the slot list, and that
> the original `sf_` capture came from a run whose config left the prelaunch UI up. That is a
> hypothesis for the next session, not a finding.
>
> The `th3_` run did reach their UI, but the surface came back **736 px wide** (the prelaunch UI resizes
> it) and the frames show the PLAY screen, not the save-slot list.
>
> So the measurement below stands on the capture taken while the file existed, and it is recorded in
> full so it can be checked; **it is not currently reproducible.** The productive next step is to copy
> `sf_5_05_file_select.png` out the moment it exists, and to reproduce the original session's config
> rather than guessing at it. **Rule: a frame cited as evidence must be copied into
> `tests/vita3k/screenshots/` in the same session it is captured.** §7.12's own "capture is no longer
> reproducible" note is the same failure, one month earlier.

**Same screen as `sf_5_05_file_select.png`, measured.** Comparison of the centre-column dark-run
structure, which is where a menu's layout lives:

| element (centre y) | ours | theirs | offset | height ours/theirs |
|---|---:|---:|---:|---:|
| slot 1 row | 189.5 | 191.0 | −1.5 | 30 / 29 |
| slot-1 lower bevel | 264.5 | 266.0 | −1.5 | 36 / 35 |
| slot 2 top | 306.0 | 306.5 | −0.5 | 29 / 28 |
| slot 2 row | 381.5 | 382.5 | −1.0 | 36 / 36 |
| slot 3 top | 421.5 | 422.5 | −1.0 | 28 / 28 |
| Confirm bar | 529.0 | 530.0 | −1.0 | 29 / 27 |

**Six of seven structural elements match at −1.5 … −0.5 px with height ratios 1.00–1.07.** Each image
has exactly one unmatched 20-px element, and they are 398 px apart from the slot-1 row — the animated
selection highlight, on a different slot in each capture. Identical layout, one moving cursor.

**Do not quote a pixel-difference percentage for this pair.** Measured, it is meaningless:
59.8 % at the best alignment, 63.9 % after a scale search. That is not a rendering defect. It is one
animated highlighted row plus sub-pixel text edges: on a 2D screen of ~50k colours, a 1-px offset
re-lights most glyph edges and the comparison counts each as an error. The structural measurement
above is the evidence; the percentage is not.

**The detector was wrong, and it was not "fixed" by loosening it.** `compare_port_frames.py` refused
this pair at correlation +0.538 against its 0.85 bar. Lowering the bar would have been wrong, and the
numbers say why:

```
verified SAME screen  ours(file select) vs sf_5_05(file select)   +0.538
verified SAME screen  ours(name screen) vs nm_5(name screen)      +0.372  (nm_5 not even top-4)
KNOWN DIFFERENT       ours(attract loop) vs sf_5_05(file select)  +0.533
```

A same-screen pair at 0.538 and a different-scene pair at 0.533 means **no threshold separates them**.
The 32x32 global luminance fingerprint is dominated by the shared parchment board and the letterbox
edges, so every 2D menu in this game fingerprints alike. Gradient, high-pass, row-profile and
bright-fraction fingerprints were each tried and each ranked a *title screen* above the true file
select; none was shipped. Instead the tool keeps its strict bar and gains an explicit escape hatch:

```sh
tools/compare_port_frames.py ours.png theirs.png \
  --assert-same-screen "Choose a Quest Log board, slots 1/2/3 all New Quest Log"
```

The reason is required, echoed into the output and into `--json` as `asserted_same_screen`, and the
result is labelled `scene identity asserted` — the pixels are computed, the identity is the operator's
claim. A refusal without the flag is unchanged (exit 1), so the guard still catches the accident it was
built for.

**What this does and does not establish.**

- **Established:** both ports render this screen, and their menu layouts agree to about one pixel.
  That is the geometry half of §7.12's open question, and it needed no renderer change.
- **NOT established:** which port renders *better*. The pair differs in the animated highlight and in
  the button glyphs, which is by design, so there is no like-for-like pixel claim to make here. The
  §7.4b fixes (min-filter table, light normalisation) still have no pixel evidence attached.
- **Still missing:** DuskOnVita's frames. It publishes no VPK, so the three-way comparison remains two
  ports until it is built from source.

### 7.13 vitaGL vs GXM vs hybrid: what the numbers actually say (2026-11-12)

The only port that ships **both** backends is DuskOnVita, so it is the only source of a same-scene,
same-console A/B. Everything below is quoted from its `docs/benchmarks.csv` and `docs/PERFORMANCE.md`
(a clone at `/tmp/DuskOnVita`), not estimated. **None of it was measured on this port.**

#### The direct A/B: build g2c, PS Vita 1000, 444 MHz, 23 Oct

| Scene | vitaGL | GXM + G2 | delta |
|---|---:|---:|---:|
| Link's house | 20.24 | 19.45 | **−3.9 %** |
| Ordon, outside the house | 22.94 | 27.23 | **+18.7 %** |
| Faron fight | 11.71 | 14.04 | **+19.9 %** |

**It is not a constant win, and the sign flips with the scene.** Their own note says Link's house turned
out to be *GPU-bound at full resolution* — at 100 % the GPU is the limit, so a cheaper CPU-side backend
buys nothing and GXM's slightly different submission costs 4 %. The two scenes that gain ~19 % are the
ones limited by the GX worker or the main thread.

**This A/B is not a pure backend swap, and that matters.** `g2c-gxm` carries G2: resident geometry
(`--vita-gxm-resident`), packed 8/16-bit vertex formats (`--vita-gxm-compact`) and native
quads/strips/fans (`--vita-gxm-primitives`). There is **no published "GXM without G2" run**, so the
backend's own share of that +19 % is unmeasured. Someone reading "+19 % for switching to GXM" would
overstate the backend by an unknown amount.

#### Why the ceiling is around 20 %, from the frame itself

The GX worker's 37.9 ms per frame (g2c, Link's house) decomposes as:

| part | ms | share |
|---|---:|---:|
| **backend draws** | **10.9** | **28.8 %** |
| pipeline lookup | 8.2 | 21.6 % |
| XF: matrices, lights | 6.0 | 15.8 % |
| BP: TEV, blend, textures | 6.0 | 15.8 % |
| draw parsing | 2.9 | 7.7 % |
| CP: vertex arrays | 2.3 | 6.1 % |
| draw setup, command loop, static lists | 1.6 | 4.2 % |

A backend swap can only touch the **10.9 ms** of backend draws. Even making them free cannot exceed
about a quarter of GX worker time, which is why the measured win is ~19 % on worker-bound scenes and
negative where the GPU binds first.

And the backend is not where the frame goes. Their first console profile (v61, one core, 333 MHz,
154 ms/frame) put **vertex preparation at 66 ms = 42.9 %**, GX decode 31 ms = 20.1 %, *vitaGL calls*
25 ms = 16.2 %, J3D draw building 20 ms = 13.0 %, simulation 12 ms = 7.8 %.

**So the renderer is the third-order lever, not the first.** The first-order levers are the ones that
move work off one core or remove it: threading, resident/static geometry, and the vertex path.

#### What each non-renderer lever was worth (same backend)

Ordon village centre, 10 Nov, one thing added at a time:

| change | FPS | vs control |
|---|---:|---:|
| control (every new speed-up off) | 12.29 | — |
| GX worker speed-ups only | 13.05 | +6.2 % |
| + direct uniforms + actor fast paths | 13.87 | +12.9 % |
| + Performance (picture-only, opt-in) | 15.60 | +26.9 % |

Frame overlap alone, console, Ordon at 75 %: **20.5 → 25.9 FPS (+26 %)**. Clocks 333 → 444 MHz is ~1.33×
on paper. Those are larger than the backend swap and they compose.

#### Hybrid

Two different things get called "hybrid", and only one is interesting:

1. **Both backends present, selectable at run time** (what DuskOnVita actually does). Cost is one
   function-pointer table; value is a fallback when one backend cannot start — their GXM writes
   `gfx_init_failure` and exits if `sceGxmProgramCheck` or a pool allocation fails. That is a
   robustness argument, not a performance one.
2. **One scene drawn by both, split by pass.** This is the only hybrid with a performance story, and it
   comes straight out of their gap list: their GXM backend does **not** yet do off-screen passes and
   EFB copies — shadows, heat haze, the minimap — and some indirect-texture effects. Those are exactly
   the paths vitaGL already implements correctly here (§7.6: 11,338 resolve ops, ~11/frame, all reaching
   `draw_quad`). A hybrid that keeps GXM for the main pass and vitaGL for EFB-copy/off-screen passes
   would run two GL contexts' worth of state on a 128 MiB GPU budget with no measured data to justify
   it. **No one has measured it, and this document should not imply otherwise.**

#### What this predicts for `tp-vita`, labelled as a prediction

This port is **vitaGL only, single-threaded, no GX worker, no frame overlap, no resident geometry, no
static display lists**, and it does not request a clock change. The closest measured point is
**v61: 6.34 FPS in Link's house and 7.39 outside at 333 MHz on one core with every draw**. That is the
band to expect from the current build on hardware — not the 12–28 FPS the other port reports, because
none of the levers that got it there exist here.

**What is NOT claimed:** no hardware run of this port has ever happened, so all of the above is
transfer from a different codebase. The 151.94 ms/frame figure in §7.6 is a Vita3K number and says
nothing about a Cortex-A9.

#### 7.13.1 What "G2" means, and a contradiction in their own docs (2026-11-12)

**"G2" is not a switch.** It is the **third build stage of the GXM backend**, and their
`docs/ARCHITECTURE.md` names the three stages explicitly:

| stage | what it added |
|---|---|
| **G0** | the backend table and the per-frame draw digest — the interface, no drawing |
| **G1** | the backend itself on SceGxm: display buffers, CDRAM texture pool, uncached vertex ring, shader patcher, GXP cache, state |
| **G2** | the three optimisation switches below |

They are independent, GXM-only, and **all default `off`** in the options table
(`docs/PERFORMANCE.md:107-109`):

| switch | values | default | what it does |
|---|---|---|---|
| `--vita-gxm-resident` | on/off | **off** | certified geometry and static-list indices stay in a GPU arena instead of being copied every frame |
| `--vita-gxm-compact` | off/on/colors | **off** | integer positions, normals and texture coords keep their GX format, scaled exactly in the vertex shader; `colors` also sends RGBA8 colours as bytes |
| `--vita-gxm-primitives` | on/off | **off** | quads, strips and fans drawn natively, without index lists (patch `aurora/0008`) |

**They are not GXM-specific in spirit, only in implementation.** Each removes a copy, a conversion or an
index list — the same three things a vitaGL port could also stop doing. That is the transferable idea;
the flags themselves are not portable.

**The contradiction, and how it resolves.** The README's A/B table says "GXM + G2" and its caption says
g2c is "the native renderer with resident geometry, packed vertex formats and native primitives", while
the options table gives all three a default of `off`. Both are true in different modes, and
`platform/vita/main_vita.cpp` says which:

- the **play profile** (`PlayProfile[]`, line 89) contains `--vita-gpu=gxm --vita-gxm-resident=on
  --vita-gxm-compact=colors --vita-gxm-primitives=on`. A launch that selects **no** renderer option gets
  this implicitly (`implicitProfile`), so on a normal launch all three are **on**.
- selecting **any** renderer option own-chooses them, and then each falls back to its table default,
  **off**. `--vita-profile=off` turns every path off.

So "G2 off" is the *explicit-option* state, not the shipped default. The `g2c-gxm` benchmark rows carry
the bare note `--vita-gpu=gxm`; whether the profile added G2 on top depends on whether that run counted
as `implicitProfile` or `explicitRenderer`, which the CSV row does not record. **That is a gap in their
record, not a conclusion here: the +18.7 % / +19.9 % figures may or may not include G2.**

**Where the backend's own share IS separable, and it is small.** Their play profile carries two later
GXM additions with console numbers attached (`main_vita.cpp:126-128`): the pipeline memo, which took
the worker's pipeline step from 10.7 to 5.1 ms in Ordon centre and 13.6 to 3.7 ms in Hyrule Field, and
direct default-uniform writes, which took the **backend step from 24.0 to 23.1 ms — about 1 ms/frame**.
A ~1 ms change on a 24 ms backend step is the resolution a pure backend optimisation actually has, which
is consistent with §7.13's ceiling argument: backend draws are 10.9 ms of a 37.9 ms GX worker.

### 7.14 Which backend would theoretically be faster, and how much (2026-11-12)

**This is a model, not a measurement. Every number is tagged with whether it was measured, and every
assumption is named so it can be attacked.** The question "which backend is faster" has a short answer
and a useful answer, and they differ.

#### Short answer

**Direct GXM, by roughly 1.1x** — and only on scenes whose bottleneck is the GX worker. It is *slower*
where the GPU binds first. Switching backend is the **third-largest** lever available, behind threading
and the vertex path.

#### Why: the frame does not spend its time in the backend

Measured, v61 on the console, one core, 333 MHz, 154 ms/frame (`DuskOnVita docs/PERFORMANCE.md`):

| component | ms | share | what a backend swap can do |
|---|---:|---:|---|
| vertex preparation | 66 | 42.9 % | **nothing** — happens before the backend |
| GX command decode | 31 | 20.1 % | **nothing** — happens before the backend |
| **vitaGL calls** | **25** | **16.2 %** | this is the whole target |
| J3D draw building | 20 | 13.0 % | **nothing** |
| game simulation | 12 | 7.8 % | **nothing** |

The GL layer is 25 ms of 154 ms. Even making it **free** caps the win at 1.19x. Removing 60 % of it —
which is roughly what shedding GL's state machine, error checks and redundant bind validation gets you,
**and that 60 % is an estimate, not a measurement** — gives:

| change | frame | speedup |
|---|---:|---:|
| backend only (vitaGL calls −60 %) | 139.0 ms | **1.11x** |
| + resident/packed geometry (vertex prep −75 %) | 89.5 ms | **1.72x** |
| + GX thread and frame overlap (2 cores busy) | ~54 ms critical path | **~2.9x** |

Those rows are **Amdahl arithmetic on measured components with assumed removal fractions**; only the
154 ms total and the per-component split are measured. The ordering, however, does not depend on the
assumptions: a layer that is 16 % of the frame cannot beat changes that address 43 % and 20 % of it.

**A second, independent ceiling agrees.** In g2c's GX worker, 37.9 ms total, **backend draws are 10.9 ms
(28.8 %)**. If the worker is ~76 % of that frame, a completely free backend buys at most **~22 % of frame
time** — and that is the optimistic bound for a backend that does nothing.

#### Where direct GXM wins, and where it loses

Measured, g2c, 444 MHz, same build, same scenes:

| Scene | vitaGL | GXM + G2 | sign |
|---|---:|---:|---|
| Link's house (GPU-bound at 100 %) | 20.24 | 19.45 | **−3.9 %** |
| Ordon, outside (worker/main-thread-bound) | 22.94 | 27.23 | **+18.7 %** |
| Faron fight | 11.71 | 14.04 | **+19.9 %** |

So "which is faster" is **scene-dependent by sign**, not merely by magnitude. Two mechanisms explain it:
the gain appears only when the CPU side is the limit, and GXM's fixed submission cost is not free, so on
a GPU-bound scene the cheaper-looking path can come out slightly behind.

#### Which backend for `tp-vita` specifically

`tp-vita` is **vitaGL only, single-threaded, no GX worker, no frame overlap, no resident geometry, no
static display lists**, and **does** request 444 MHz (see the correction below). Two consequences:

**CORRECTION (2026-11-16):** the sentence above said this port "requests no clock change". It does:
`vita/src/main.cpp:68-71` sets ARM 444 MHz, bus 222, GPU 222, xbar 166. The comparison to v61 -- which
was taken at stock 333 MHz -- therefore **understates** our headroom rather than overstating it, and the
444-vs-333 factor is already banked. The ranked lever order is unaffected; see §7.20.

1. **Its GL layer is a smaller share than 16 %.** The 25 ms in that table is the *whole* GL call cost
   inside a pipeline that had already been optimised; our replay path also does decode, state caching
   (`sCache`), fog-range LUT rebuilds (§7.3b) and shadow-list work in the same thread. Moving to GXM
   would not delete any of that.
2. **The lever that dominates here is not the backend at all.** Our nearest measured anchor is v61 —
   6.34 FPS in the house, 7.39 outside — which is *exactly* the single-core, stock-clock,
   every-draw configuration this port ships in. The gap between that and the other port's 12–28 FPS is
   almost entirely **threading, resident/static geometry and the vertex path**, i.e. the 1.7x and 2.9x
   rows, not the 1.11x row.

**Theoretical answer, stated plainly:** a direct-GXM rewrite of this port's renderer is worth roughly
**1.1x**, and it is the most expensive of the three levers because it means reimplementing the GL
surface the replay path uses. A GX worker plus frame overlap is worth **more** and touches far less code.
Resident/static geometry is worth more again. If the goal is frames, **the backend is the wrong thing to
rewrite first**.

#### What would falsify this

- A run showing our per-frame `vitaGL` time is a much larger share than 16 %. Instrument the replay's GL
  calls the way §7.9 instrumented decode, and the model is checkable without a console.
- A GXM-without-G2 benchmark, which does not exist in their repository (§7.13.1) — that is the number
  that would separate the backend from the G2 switches.
- Any hardware run at all, which this port has never had.

### 7.15 The x20 Link heap was live on Vita, and is now x3 — 10.79 MiB back (2026-11-16)

**Trigger:** DuskOnVita published **v0.9.0 on 2026-11-14, the first release with a VPK**. Its commit
`1b6472c4` ("Crash fix: Link's own heaps are three times the original's size, not twenty", patch
`dusklight/0077`) describes a failure mode that matches ours, and the check found it live here.

#### The defect was ours too, and the guard that should have stopped it was defeated

`daAlink_c::setOriginalHeap` multiplies Link's two heaps by **20** under `#if TARGET_PC`:

```cpp
u32 size = ROUND(i_size, 16);
#if TARGET_PC
    size *= 20;   // "prevent mods from crashing with higher-quality models"
#endif
```

**This tree defines `TARGET_PC=1` for the Vita build as well**
(`cmake/GameABIConfig.cmake:7`), so a guard that reads like "PC only" was **live on Vita**. Link's
archive heap became `0xA2800 * 20 + 0xA0 = 0xCB20A0` = **12.70 MiB**, taken in one piece from the game
heap every time Link is created — a stage change, or the restart after a fall.

Our own `dvd_trace.txt` had this recorded all along:

```
JKRExpHeap: create: size=0xCB20A0 aligned=0xCB20A0 parent=gameHeap parentFree=0x14385B0
```

Nobody connected `0xCB20A0` to `0xA2800 * 20`. The lesson is §8 trap 16's, one layer up: **a
platform guard is only real if the macro is not also defined for your platform** — check the compile
definitions, not the `#if`.

#### Fix, and the proof

`#if TARGET_VITA` now does `size *= 3` before the `#elif TARGET_PC`. Measured in the same run, same
harness, only the multiplier changed:

| | request | MiB |
|---|---:|---:|
| before | `0xCB20A0` | 12.70 |
| after | `0x1E78A0` | **1.90** |

Both decompose exactly as `base * N + 0xA0`: `0xA2800*20 + 0xA0` and `0xA2800*3 + 0xA0`. The shield
heap moved the same way, `0x7D000*3 = 0x177000`. **10.79 MiB reclaimed per Link creation.** No
`fatal.txt`, no `crash.log`, frames render normally (all five captures `738x544` at the expected bbox),
and the host suites are unchanged: e2e 91 scenes / 14,266 checks / 0 failed, texture 3,780/0, crash
PASS, printf clean.

**Not proven:** that this was the §7.1/§7.12b abort. Those runs died in the **attract loop** while
streaming demo resources, before Link exists, and the failure carried no size. This fix removes a real
12.70 MiB single-block request that could strand a stage change or a post-fall restart; whether it
retires the attract-loop abort is **untested**, and the `0xBF18265F` corrupted-size case remains
unexplained. Do not write "the heap abort is fixed".

#### Their v0.9.0 console numbers, for the record

PS Vita 1000, 13 Nov, default "Speed-ups: Original": Snowpeak Ruins 19 FPS, Kakariko 18–19, Ordon
village 14 standing / 16 walking, Hyrule Field enemy areas 14.5, Palace of Twilight 14, Ordon widest
view 10. With "Performance": Ordon village mostly 30, centre 22.6–24.4, Forest Temple 17.5–26.2.

Also recorded by them and relevant here: **the minimap is not drawn in play**, and neither are Link's
real-time shadow, water, bloom, depth of field or heat haze — the same class of gaps as §7.4b's list.
Their known issue "some textures flicker" is still open, and Shader compilation stalls ~0.4 s per new
stage with no warm-up bundle shipped.

#### The §7.12 blocker is closed, and one class of comparison is now possible

§7.12 said a three-way image comparison could not be produced because DuskOnVita "publishes no release
and no VPK". That is no longer true: `DuskOnVita.vpk` (11,867,758 bytes, SHA-256
`14c67742…8ed6b3`, verified against the published digest) is downloadable. **It does not make the
perf comparison possible** — Vita3K cannot resolve frame rate (§7.8) — but it does make the
**same-scene pixel comparison** possible, which is the one §7.12 was blocked on.

**§7.16 narrows the last sentence**: the capture is now possible and has been done, but the two ports
render at different widths (theirs 960, ours 738), so a same-scene pair is a comparison of two render
resolutions, not a like-for-like quality check. What the release does give is a runnable third port
whose structure can be inspected.

---

### 7.16 DuskOnVita 0.9.0 runs here, and it does NOT letterbox — §7.12's geometry claim was wrong (2026-11-16)

**DuskOnVita v0.9.0 (2026-11-14) is the first release with a VPK**, so the third port is finally
runnable on this machine. It installs, boots, and reaches live 3D gameplay in Vita3K.

#### Setup that was needed, and two traps that each cost a run

1. `DuskOnVita.vpk`, 11,867,758 bytes, SHA-256 `14c67742…8ed6b3` — **verified against the digest in the
   release notes** before use.
2. Their disc path is `ux0:data/twilight-vita/`. **A symlink there does not work**: their reader reports
   `disc_search: ux0:data/twilight-vita/game.iso=no_image`, because Vita3K does not resolve a symlink
   through the guest filesystem. A **real copy** of the ISO is required (1.4 GB, reflinked).
3. `libshacccg.suprx` must be at `ur0:data/external/`, not only `ur0:data/` — §7.4b records the same
   requirement for the other third-party VPK.
4. **The first run crashed in the emulator, not in the app**: `Unhandled SIGSEGV at rip 0x0000708900aeb38d`
   / `Unhandled access to 0x460000000`. That RIP is a **host** address, i.e. a Vita3K fault during their
   GXM backend start-up. The next run with identical inputs went through. Recorded as a per-run emulator
   flake, not a defect to chase.

Their own receipts show the run is real: `build_version: DuskOnVita 00.90 build 72d8bc7d (2026-11-14)`,
`arm_mhz: 444`, `gpu_mhz: 222`, `game_main_enter`, and steady frame windows
(`prof_frame_p50_us: 33257`, `prof_frame_p95_us: 33882` around the 33.334 ms tick).

#### The measured finding: their picture is 960 wide, ours is 738

Content box measured from the captures, non-black span inside the 1280x800 window:

| port | content box (window coords) | width | height |
|---|---|---:|---:|
| DuskOnVita 0.9.0 | `(160,128)-(1119,671)` | **960** | 544 |
| tp-vita | `(271,128)-(1008,671)` | **738** | 544 |

Both start at the same `y` and both fill the Vita surface vertically. **Theirs fills the full 960 px
surface; ours is pillarboxed to 738 px with 111 px bars on each side** (271 = 160 + 111, so ours is
inset 111 px inside the same surface rectangle). Three of their four captures agree exactly on
`960x544`; `dov090_4_04.png` reports only `960x408` at `y=205`, which is either a letterboxed cutscene
or a dark scene — **this detector cannot tell a black bar from a black picture**, so that one frame is
not evidence either way and no conclusion rests on it.

Numbers above are what `tools/compare_port_frames.py` itself reports (`_content_box`, "any non-zero
channel"), not a re-measured guess: an ad-hoc harness using a `max > 8` threshold reported `960x391` at
`y=206` for that same frame, because a louder threshold clips the near-black edge rows. **When quoting a
content box, quote the tool's function** — otherwise the geometry in this document stops matching the
geometry the tool computes, and a later session cannot reproduce it.

**This corrects §7.12**, which asserted the geometry was "NOT the blocker, and that was checked",
because "both ports letterbox to the same 738 px width at the same x". That was inferred from *source*
(`tests/gxm_state_test.cpp`'s `viewport(118,0,724,544)`) and from other ports' captures — never from a
DuskOnVita frame, because none could be captured then. Now one can be, and **the shipped build does not
use the viewport its own test file states**. `KNOWN_GEOMETRY` in `tools/compare_port_frames.py` is
corrected to the measured box, and the `--port-status` note that they "ship no VPK" is corrected too.

**Consequence for any future diff:** our 738 px is genuinely *inside* their 960 px, so the tool's
intersection is legitimately 738 px wide — but a viewport difference this large means the two pictures
are **scaled differently**, not merely offset. A pixel difference between them measures the resolution
mismatch before it measures anything about either port's correctness.

#### What this does and does not unblock

- **Unblocked:** the third port produces frames here, so §7.12's "two of three can be run" becomes
  three of three, and structural questions about their UI can be answered by looking at it.
- **NOT unblocked:** a relative-quality claim. The ports render at different widths, so a same-scene
  pair differs first by that. The scene still has to match as well: their run reaches gameplay on its
  own schedule, ours needs the frame-counted injector (§7.12c).
- **Still not measurable:** performance. Vita3K's noise is far larger than the differences (§7.8), and
  their `prof_frame_p50_us` of 33.257 ms is the emulator pacing to the 30 Hz tick, **not** a hardware
  frame time. Do not quote it as one.

### 7.17 Celosia: a second Vita port with a patched vitaGL, and what is actually liftable (2026-11-16)

[`HenryKun55/celosia`](https://github.com/HenryKun55/celosia) — GPL-3.0, 76 stars, created 2026-10-02,
last push 2026-11-15 — is *Shadow of the Colossus* (PS2) statically recompiled to C++ with PS2Recomp,
rendering through Metal on Apple Silicon and **OpenGL ES / vitaGL on the Vita**. It is a second worked
precedent for the static-recompilation route, and it is the most detailed public record of vitaGL
behaviour on real hardware that this project has found.

#### It patches vitaGL, and the base is verifiably our pin's child

`PS2Recomp/ps2xRuntime/platform/vita/vitagl/celosia.patch`: **2,131 lines, 16 files, 107 hunks**,
prefixing its additions `vglCelo*`. The base was not taken from a comment — the patch's own blob hash
for `source/buffers.c` (`9ef4257`) resolves to vitaGL commit **`92888a7e`, 2026-09-24** ("Add optional
vertex layouts caching for shaders pipeline"). Our `vdpm` pin is **`2bdbe89`, 2026-09-23**, and

    git merge-base --is-ancestor 2bdbe89 92888a7e  ->  true
    git rev-list --count 2bdbe89..92888a7e          ->  1

so **our installed r1488 is the direct parent of their patch base.** That is the best possible
alignment, and it still does not make the patch applicable: applied with `--fuzz=0` to a pristine
`2bdbe89`, `buffers.c` fails outright and `gxm.c` loses 2 of 12 hunks. Applied to **its own declared
base** `92888a7e` it *still* fails 3 hunks and reports "patch unexpectedly ends in middle of line".
**The file is a published diff, not a cleanly applicable patch series**, so treat it as a *reference
for what to change and why*, not as a vendor drop.

#### Their measured wins, which are the part worth reading

Every item below is theirs, measured on their port, and none is verified here:

| Their switch / patch | What it fixes | Their number |
|---|---|---|
| `SOTC_VGL_CIRCULAR_KB=3072`, `_IMMEDIATE_KB=64` (from 32768/4096) | the defaults are grossly oversized; immediate mode is unused but was 4 MiB | **29 MB** of main-RAM + phycont pools back |
| `vglCeloOwnMips` (`SOTC_GL_OWN_MIPS`) | vitaGL **ignored** the texels of mip levels >= 1 and downscaled level 0 with `sceGxmTransferDownscale`, an async GPU transfer | correctness |
| `vglCeloTexImageSwizzled` | uploads in GXM Morton order instead of rows | texel locality |
| `SOTC_GL_KEEP_LAYOUT` (`vglCeloLeanDraw` bit 2) | every mesh-pool switch re-patched the vertex program | **~170 re-patches/frame** avoided |
| `celo_unif_copy` (`vglCeloLeanDraw` bit 0) | default uniform buffers copied via the faster path instead of `sceClibMemcpy` | 0.3–3 KB × ~1,500 draws/frame |
| `vglCeloDepthTiled` | depth/stencil in GXM's tiled layout | **26.2 → 10.1 MB** depth traffic/frame |
| `vglCeloUniformPool` | the uniform pool could come round onto uniforms the GPU had not read | ring hazard |
| `SOTC_GL_KEEP_TARGETS` | **Vita3K** looks textures up in its colour-surface cache by address + stride and keeps a freed surface, so a later texture put there drew the old picture | emulator-only |
| `SOTC_VGL_POOLS=console` | sizes vitaGL's pools as on a console inside Vita3K instead of from the emulator's larger free memory | testing honesty |
| `SOTC_GL_INFLIGHT87` | GXM's **16 MB parameter buffer** holds every scene in flight; 2–3 frames in flight can overflow it into partial renders | they measured "matched, GPU idle between frames", so it **stays off** |

**The 16 MB GXM parameter buffer is a hard structural bound on frames in flight**, and it is independent
of anything §7.13–7.14 counted. It is a third constraint on the renderer alongside bandwidth and draw
calls, and §7.14's model does not include it.

**One result is a negative and is more valuable than the positives:** they implemented the
vitaGL-to-GXM **hybrid** by patching vitaGL itself, not by bolting a second backend beside it, and they
still do not ship it as the default. That is consistent with §7.13's finding that the backend is not
where the frame goes, and with §7.14's ranking of the levers.

#### Tooling worth adopting, which does not depend on their patch

- `tools/vita_hot_order.py` + `game/overrides/vita_hot_order.ld` — generates a linker script that places
  the **hottest functions at the start of `.text`**, from Vita3K profiles. Cheap, and it applies directly
  to us: our link order is whatever CMake emits.
- `game/pgo/vita-pgo.tar.xz` — they ship **PGO profile data for the Vita build**. We have never tried PGO.
- `tools/vita_profile.py`, `tools/vita3k.sh` — their emulator/profile wrapper, the same shape as our harness.
- `-DPS2X_VITA_FUNCTION_SECTIONS=OFF` to A/B the hot-code ordering, i.e. they kept the control.

#### Do not copy their tree: it contains Sony binaries

`PS2Recomp/ps2xRuntime/vita/module/` holds **real Sony modules**, not stubs — `libGL.suprx` 1,325,945 B,
`libGLESv2.suprx` 1,995,561 B, `libIMGEGL.suprx` 97,629 B, `libgpu_es4_ext.suprx` 39,821 B,
`libpvrPSP2_WSEGL.suprx` 17,869 B. Redistributing those is the same class of problem as shipping
`libshacccg.suprx` (the skill's ground rule). Take their **patch, their tools and their numbers**; take
nothing from that directory, and do not vendor the repo wholesale.

### 7.18 `vita_hot_order.py` exists now, and the flag that makes it work is not the obvious one (2026-11-16)

Celosia's hot-code idea is worth having and does not need their patch, so it is now a skill tool:
`~/.roo/skills/vita-port/scripts/vita_hot_order.py`. It reads `<profile>:<elf>` pairs (Vita3K's
`<thread> <pc> <lr> <weight>` samples), attributes each PC to a symbol with `nm -n -S`, normalises each
profile so a long run cannot outvote a short one, and writes a linker-script fragment listing the
functions hottest first.

**Verified on this machine, not assumed:**

- The generator resolves PCs correctly and orders by weight. A four-function fixture profiled
  heavy-to-light produced the emitted order `main, fn_b, fn_c, fn_a` -- descending weight, with the
  unsampled function last. Reproduced twice.
- `-Wl,-T,<fragment>` **works**: the linked symbols came out at offsets 0/4/8 in the requested order,
  with `.rodata`/`.data`/`.bss` still present (36 sections).
- `-Wl,--section-ordering-file=<fragment>` **does not work for this**, and the error is specific:
  `output section '.text.<name>' must already exist`. GNU ld orders *output* sections; it cannot order
  per-function input sections. `SECTIONS { }`, bare names and `SORT_BY_NAME(...)` are all syntax errors
  to it. LLD accepts that flag with a different format, which is how the wrong recipe spreads.
- `-Wl,--sort-section,name` reorders but alphabetically, not by heat.

**Caveat before we use it on `tp-vita`:** a `-T` fragment can displace the default script's entry-point
and segment handling. On a bare test link the entry point came out `0x0`. Our VPK goes through
`vita-elf-create` and `vita-make-fself`, so the fragment has to be checked against the whole packaging
chain, not just the ELF.

**What we still lack** to apply it here: `-ffunction-sections` is **not** enabled (our ELF has one
`.text` and zero `.text.*` sections), and nothing in `tests/vita3k/` captures a CPU profile. So this is a
prepared tool with a verified mechanism, not a measured win. Two prerequisites, both small: add
`-ffunction-sections` behind an option, and run Vita3K with its profiler on for a scripted route.

### 7.19 Whether hot-ordering buys anything HERE: the veneers are real, but only 47 are hot (2026-11-16)

§7.18 prepared the tool. This measures whether it is worth running, and the answer is narrower than the
premise. **Our `.text` is 18.55 MiB against a 16.00 MiB Thumb-2 `BL` reach**, so the constraint is real
and the binary is 2.55 MiB over it. `arm-vita-eabi-nm` finds **316 `_veneer` symbols**, i.e. the linker
genuinely had to stub 316 call sites.

**But a veneer is not evidence that a hot call is paying for it.** Classifying all 316 by resolved target:

| Class | Count | Hot? |
|---|---:|---|
| C++ runtime: exceptions, iostream, locale, chrono, filesystem | **156** | no |
| other libc (`rename`, `vsnprintf`, `atan`, `remainderf`, `__init_vita_heap`, `__sinit`) | 72 | mostly no |
| **libc inner loops** (`memcpy`, `memset`, `malloc`, `fwrite`, `strtol`, `fflush`, `fgets`) | **31** | **yes** |
| **libgcc** (`__aeabi_idiv`, `__aeabi_ldivmod`, `__aeabi_l2d`, `__aeabi_ul2d`, `__aeabi_d2ulz`) | **11** | **yes** |
| **`operator new` / `delete`** (`_Znwj`, `_ZdlPv`, `_Znaj`, `_ZdaPv`) | **5** | **yes** |

**So roughly 47 of 316 are on hot paths**, and they are exactly the ones that hurt: allocation, `memcpy`,
integer division and float conversion are called from inside inner loops.

**The mechanism is not "hot code is far from libc".** Measured: `memcpy` is at `0x82273cb8`, i.e.
**18.45 MiB into `.text`** — the very end. Every call site in the **first 2.45 MiB** of `.text` is out of
range of it. The veneers cluster around `0x813f8xxx` (≈4.0 MiB in), i.e. the boundary where code
crossing the limit begins.

**Which means the fix is narrower than a profile-driven hot set.** Anything that moves the ~47 hot
libc/libgcc/new-delete targets from the end of `.text` to the **start** puts them within reach of the
whole 18.55 MiB (18.55 < 2 × 16), and removes those veneers without any profile. That is a link-order
fix for a handful of well-known libraries, not a hot-code ordering problem. A CPU profile is still
needed for the *game* code case, but the game code is not what is being veneered.

**What is NOT claimed:** that removing a veneer is worth a measurable frame time. A veneer is one extra
branch through a stub — an extra instruction fetch and an extra indirect target. On a Cortex-A9 with a
small BTB that is plausibly a few cycles per call, so 47 sites in inner loops is a real but small
effect. **Vita3K cannot resolve it** (§7.8), so this must be measured on hardware or not claimed at all.

**Cheapest experiment, in order:**
1. ~~Order the hot libraries first (no profile, no `-ffunction-sections`)~~ -- **DONE AND IT FAILED,
   see §7.19b**: archive-level ordering moved 4,921 functions and made the hot veneer count worse
   (58 -> 90). Do not repeat it.
2. The remaining option is function-granular: `-ffunction-sections` plus a CPU profile, naming the ~40
   hot targets individually through `vita_hot_order.py` (§7.18).

### 7.19b Experiment 1 was run and it FAILED: moving libc first made the hot veneers worse (2026-11-16)

§7.19 predicted that moving the hot libc/libgcc/new-delete code to the front of `.text` would remove
roughly 47 veneers **without any profile**. That was tested on the real binary and it is **wrong**.

**Method, a controlled A/B.** The baseline ELF was saved, then relinked from the *identical* object
files by replaying `build/CMakeFiles/tpv_vita.dir/link.txt` with one flag inserted:

    SECTIONS { .text.hotlibs : {
        *libc.a:*(.text .text.*)  *libgcc.a:*(.text .text.*)
        *libsupc++.a:*(.text .text.*)  *libstdc++.a:*(.text .text.*)  *libm.a:*(.text .text.*)
    } }
    INSERT BEFORE .text;

`INSERT BEFORE` was chosen deliberately over the plain `-T` fragment of §7.18, because it merges with the
default script instead of replacing it -- and it works: `.text.hotlibs` was created at `0x81000010`,
before `.text` at `0x81099580`, with 4,921 functions moved into it.

**Result: worse.**

| | total `_veneer` | **hot** veneers | cold veneers |
|---|---:|---:|---:|
| baseline | 316 | **58** | 258 |
| libc-first | 310 | **90** | 220 |

The total dropped by 6, but the number that matters **rose by 32**. Hot veneers removed were only
`_ZdlPv` and `__aeabi_d2ulz`; hot veneers *added* were `_ZnwjRKSt9nothrow_t`, `__aeabi_d2lz`,
`__aeabi_f2lz`, `__aeabi_uidiv`, `calloc`, `realloc`, `sprintf`, `strcpy`.

**Why it failed, and this is the real lesson.** `libc.a` is not a pile of small leaf functions. It is
mostly stdio, locale, time and filesystem code, and moving *the whole archive* to the front moved that
bulk **between** the game code and where `memcpy`/`malloc` had been. The 628 KB that went to the front
was not the hot part. `memcpy` and `malloc` did **not** move at all: in the experiment both are still in
section 4 (`.text`) at `0x82273b99` / `0x82272e49`, i.e. still ~18.4 MiB out, still out of range of the
first 2.4 MiB of code. So the change bought nothing on the targets it was aimed at and displaced other
code into new long-branch relationships.

**What this kills and what it does not:**

- **Killed:** the §7.19 claim that this needs no profile and no `-ffunction-sections`. Archive-level
  ordering is too coarse -- it moves 4,921 functions to reach perhaps 40, and the collateral damage
  exceeded the gain. A 316/58 baseline that goes to 310/90 is a regression, not a win.
- **Not killed:** the underlying constraint. `.text` is still 18.55 MiB against a 16.00 MiB reach, and
  `memcpy` is still 18.45 MiB in. The problem is real; this attempt at it was not good enough.
- **The honest next step, if anyone wants it:** function-granular ordering -- `-ffunction-sections`, a
  CPU profile, and `vita_hot_order.py` (§7.18) naming the ~40 hot targets *individually* instead of
  naming the archive. That is the experiment this one was a cheap proxy for, and the proxy failed.

**State left behind:** the build was restored from the saved baseline and verified (316 veneers,
`memcpy` back at `0x82273cb9`), and `git status` is clean -- **no source change was made**. The two ELFs
and the fragment are in `/tmp/exp1/` (`tpv_vita.baseline`, `tpv_vita.experiment`, `hotlibs.ld`,
`link.txt`) if someone wants to re-measure.

### 7.19c Is any of this worth a frame? No — the arithmetic says ~0.05 % (2026-11-16)

§7.19 and §7.19b established that the constraint is real (`.text` 18.55 MiB vs a 16.00 MiB reach) and that
the cheap fix failed. This prices the remaining idea, so nobody spends a day on it for nothing.

A veneer costs **one extra branch** (caller `BL` -> stub -> `B` -> target) plus one extra instruction
fetch. At 444 MHz that is **2.25 ns per cycle**, and a 30 Hz frame is **33,333 us**:

| cost/call | 1,000 calls/frame | 10,000 | 100,000 |
|---|---:|---:|---:|
| 2 cycles (4.5 ns) | 4.5 us (0.01 %) | 45 us (0.14 %) | 450 us (1.35 %) |
| 4 cycles (9.0 ns) | 9.0 us (0.03 %) | 90 us (0.27 %) | 901 us (2.70 %) |
| 10 cycles (22.5 ns) | 22.5 us (0.07 %) | 225 us (0.68 %) | 2,252 us (6.76 %) |

**A realistic estimate.** ~40 hot veneers, each hit ~50 times a frame, at ~4 cycles: **18 us, i.e.
0.05 % of the frame.** Even the absurd ceiling — 100,000 veneered calls every frame at 10 cycles — is 6.8 %.

**Verdict: the hot-ordering work has no measurable performance value on this port.** For scale, the
levers actually measured on the sibling port (§7.13) were 18-20 % for the backend and up to 40 % for the
non-renderer work. 0.05 % is not in the same conversation, and it is far below the ~20 % noise floor of
anything measurable here — which is also why Vita3K could never have shown it (§7.8).

**So what was this worth?** Two things, neither of them frame time:

1. **A verified mechanism for a port that needs it.** `vita_hot_order.py` and the `-T` / `INSERT BEFORE`
   behaviour are confirmed on this toolchain (§7.18). It pays off where a hot set is genuinely far from
   its own dependencies — a much larger `.text`, or game code calling cold code across a big gap. That is
   not our situation: 258 of our 316 veneers are cold C++ runtime, and the hot ones are libc that we
   cannot reposition at archive granularity anyway (§7.19b).
2. **A closed question, with evidence.** Anyone (including a future session reading §7.17's enthusiasm
   about Celosia) would otherwise try this. The answer is now written down with numbers.

**Do not spend more time on code layout on this port.** If the frame budget matters, §11's answer stands:
hardware, because everything measurable here is bottlenecked on the emulator's own honesty.

## 7.20 What to implement for performance, in order, with what is already spent (2026-11-16)

Asked directly, so this is the ranked answer grounded in measurements already in this document. **Three
of the obvious levers are already spent or already closed**, and saying so first stops the next session
from re-opening them:

| lever | state | evidence |
|---|---|---|
| clocks 333 -> 444 MHz | **already done** | `vita/src/main.cpp:68-71` sets ARM 444 / bus 222 / GPU 222 / xbar 166. Not a lever, it is the configuration all our numbers were taken in |
| draw batching | **closed** | 442 draws reach only 377 calls = 1.17x, and two earlier surveys over-counted it (§7.3c) |
| code layout / hot ordering | **closed** | worth ~0.05 % of a frame; the cheap version made the hot veneer count worse (§7.19c) |
| direct GXM backend | **wrong lever first** | 1.11x Amdahl, and the most code of any option (§7.14) |

### Rank 1 -- a GX worker thread plus frame overlap. The biggest lever, and we have none of it.

Our replay runs **single-threaded**: no worker, no overlap. The sibling port's own numbers put threading
and overlap at **20.5 -> 25.9 FPS (+26 %) on console at 75 %**, *and* it composes with everything else.
The v61 profile says why: vertex preparation (42.9 %) and GX decode (20.1 %) are **63 % of the frame** and
both are pure CPU that does not need the main thread to block on it.

The work: move `vita_gl_replay`'s decode-and-submit onto a second thread, double-buffer the command/vertex
arena, and let the main thread start the next frame's simulation while the worker drains. This is the
"~2.9x" row in §7.14's Amdahl table, and unlike GXM it deletes no functionality.

**Prerequisites, both already solved elsewhere in this document:** the heap is now sane (§7.15 reclaimed
10.79 MiB, so a second stack and arena fit), and `std::thread` actually works because pthread is linked
properly (the `--undefined=pthread_cancel` block in `CMakeLists.txt`). **Read §7.1's abort history before
touching threading** -- thread-local allocation failures were the original symptom.

### Rank 2 -- resident / static geometry. Second biggest, and mostly a memory question.

Vertex preparation is the single largest component of the frame at **42.9 %**. The sibling port's
`--vita-gxm-resident` keeps decoded vertices resident instead of re-decoding per frame, and their
`+6.2 %` (GX worker speed-ups) and larger gains stack on it. Verbatim from §7.14: resident/packed
geometry is worth **more than the backend and more than threading alone** in the optimism-matched row
(vertex prep −75 % gives 1.72x on its own).

The cost is **memory, not risk**: resident geometry trades our reclaimed heap for lookups. That is
exactly why Rank 1 comes first -- it makes the frame cheaper before we spend 100 MB.

Our §7.9 vertex cache is the partial version of this and it already works: **74 % hit rate**, correct,
and it halves decode work. Extending it from "cache the decode" to "keep the decoded meshes" is the
incremental path, not a rewrite.

### Rank 3 -- GXM for the main pass only, keeping vitaGL for EFB copies. The honest hybrid.

§7.13/§7.14 say the backend is worth **1.11x** and is the most expensive lever, so do it last or not at
all. The measured case where it *does* pay is scene-dependent: **+18.7 % / +19.9 %** on worker-bound
scenes and **−3.9 %** where the GPU binds first. And their GXM backend still cannot do off-screen passes
or EFB copies (shadows, heat haze, minimap), which our vitaGL path already does correctly (§7.6).

So if this is ever done, it is the second hybrid in §7.13: GXM for the main pass, vitaGL for the
EFB-copy/off-screen passes. **Nobody has measured that, including them.** Do not start here.

### What is genuinely free, and worth doing before any of the above

- **A CPU profile of our own route.** Every percentage above is borrowed from another port. We have no
  profile at all: nothing in `tests/vita3k/` captures one. Vita3K writes one when its profiler is enabled,
  and `scripts/vita_hot_order.py` can already read the format. **Without our own split we cannot tell
  which of Rank 1 or Rank 2 to do first** -- we are borrowing a Wii/PS2 port's shape and guessing it fits.
- **Fix §7.14's stale claim.** It says this port "requests no clock change"; `main.cpp:68` does. The
  numbers are still valid, but the sentence is wrong and would mislead someone looking for a quick win.

### The uncomfortable half of the answer

Ranks 1-2 are worth roughly **1.7x-2.9x** on the sibling port at 444 MHz. But our measured anchor is
**6.34 FPS in the house / 7.39 outside** (v61-equivalent, single core), while the other ports reach
**12-28 FPS** with the same class of hardware. That gap is bigger than any single lever below, which
means the honest first question is not "what do I implement" but **"where does our frame actually go?"**
-- and we cannot answer it from here, because Vita3K understates CPU-bound guest work by roughly an order
of magnitude (§7.8).

**Recommendation: get a console.** One hardware profile turns the ranked list above from inference into
a work order. Everything else in this document is already at the limit of what the emulator can honestly
tell us.

### 7.21 Where OUR frame actually goes, measured — and it vindicates Rank 1 (2026-11-16)

Every percentage this port had quoted about its own frame was **borrowed** from another port: §7.14's
"vertex prep 42.9 %, GX decode 20.1 %" came from a Wii/PS2 profile. It is now measured.

**Instrumentation** (`vita_gl_replay.cpp`): five disjoint phases per frame, summed over the same 120-frame
window the existing probe already reports, plus a per-frame breakdown of the remainder. Boundaries are
chosen by *what could move to another thread*, because that is the decision the numbers inform:

| phase | what it covers | can it leave the main thread? |
|---|---|---|
| `decode` | GX vertex decode / line expansion into the scratch buffer | **yes** -- pure CPU, no GL |
| `submit` | the `glDraw*` calls (`submit_geometry`) | no, GL thread |
| `pass` | program/uniform/texture binds, fog-range LUT upload | mostly yes |
| `resolve` | EFB copies | no |
| `other` | remainder of `replay()`: setup, clear, per-op binds | -- |

The five sum to 100.01 % over 9 gameplay windows, which is the check that the bookkeeping closes.

#### The measurement (9 steady gameplay windows, frames 600-1680, 148-272 draws)

| phase | mean | sd | min | max |
|---|---:|---:|---:|---:|
| **decode** | **36.9 %** | 3.7 | 27.9 | 39.6 |
| submit | 14.7 % | 1.7 | 10.2 | 15.6 |
| **pass** | **38.4 %** | 6.4 | 34.6 | 55.1 |
| resolve | 1.8 % | 0.3 | 1.2 | 2.2 |
| other | 8.3 % | 1.3 | 5.5 | 9.8 |

`other` decomposed per frame: **setup 0.10 %, clear 0.79 %, tail 0.38 %.** So `other` is *not* per-frame
overhead hiding the truth -- it is genuinely small, and the four draw phases really are the frame.

#### What this says, and it confirms §7.20 rather than overturning it

1. **decode + submit + pass = 89.9 %** of a gameplay frame, and every one of those three can move off the
   critical path (decode entirely, submit is GL-thread but can be overlapped, pass mostly). **This is the
   measured justification for Rank 1: a GX worker + frame overlap has ~90 % of the frame to work with**,
   not the 23.6 % an earlier read of mine suggested.
2. **decode alone is 36.9 %**, which is within noise of the borrowed "vertex preparation 42.9 %". The
   sibling port's split generalised after all. **Rank 2 (resident/static geometry) has a real target**: it
   attacks the single largest individual phase. Our §7.9 vertex cache already halves it at 74 % hit rate,
   so the next step is keeping decoded meshes, not building a new decoder.
3. **`pass` at 38.4 % is the largest phase and was not in the plan at all.** It is binds and the fog-range
   LUT -- per-draw GL state work. That is a *third* lever the ranking in §7.20 missed: reduce state
   changes per draw. It is also exactly what the §7.3c batching survey was measuring from a different
   angle, so its evidence is reusable even though batching itself is closed.

#### The mistake that produced a wrong answer first, recorded deliberately

An earlier pass of this measurement reported **decode 3.3 %, other 59.7 %** and I wrote a section saying
Rank 2 was dead. That was measured on the **wrong scene**: the modal frame in that run had 240 draws but
was a *menu/loading* composition, and the run's gameplay windows were classified as "huge" outliers and
excluded. Re-cutting the same log by frame range instead of by a draw-count heuristic gave the table above.

**The lesson is the one §7.8 already records, committed again**: a population chosen by one threshold
(`draws == mode`) is not the population you meant (playable gameplay). When a phase split decides a work
order, state *which frames* were averaged and check the split against the frame count and draw count
before drawing a conclusion from it.

**What is still not claimed:** these are emulator ratios, not console ratios. Vita3K runs guest ARM far
faster than a Cortex-A9 on some paths, and the 91.6 ms/frame mean here is emulator-paced. The *shares* are
the useful output and they are the best evidence available without hardware. §11's conclusion is unchanged:
a console profile is what converts this into a work order.

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
19. **At most 10 images may be in one model prompt.** Reading 11 or more PNGs in a single turn makes the
    whole request fail with HTTP 400 `At most 10 image(s) may be provided in one prompt` and the session
    must be resumed, losing the turn. This killed two sessions (2026-11-11, 2026-11-12). Read screenshots
    in batches of **10 or fewer per turn**, and prefer the numeric decode in §3 — it needs no images at
    all, so a frame can be measured without spending any of the budget.
20. **Blank frames are timing evidence, not failures.** A 263-byte `mode=1` capture means the surface
    had not been presented yet. Three blank shots at 20 s / 23 s / 26 s are what proved that a START
    injected at frame 300 lands inside the boot -> title transition and is discarded (§7.12c), which is
    why a single press did nothing while three presses worked. Keep them in the record.
21. **A source claim that fits the symptom is not a verified cause — check the launcher, not only the
    callee.** §7.12c recorded that `--dvd` made their build skip the prelaunch flow, from reading
    `m_Do_main.cpp`. The harness passes no `--dvd` at all (`in_container_script.sh:32`). One grep in the
    launcher would have caught it. When a cause names an input, find where that input is supplied.
22. **A shallow clone with one commit cannot answer "is there anything new upstream".** The
    `/tmp/DuskOnVita` copy is `is-shallow-repository: true` with a single squashed commit, so
    `git log` shows one entry and `git fetch` shows no change *either way* — both read as "nothing new"
    whether or not upstream has moved. Measured 2026-11-12: the API-published tip is `6bf5f57d`
    (2026-11-10), the same commit the clone holds, and the repo has one issue (#1, closed) and no open
    PRs. Get the tip from the GitHub commits API, not from a shallow clone's log.
23. **Never tune a detector until it agrees with you.** `compare_port_frames.py` refused a verified
    same-screen pair at 0.538 while scoring a known-different pair 0.533 (§7.12c). Lowering the
    threshold would have admitted both. The honest move is to keep the strict guard and add an
    explicitly-labelled operator assertion — and to state in the report which half of the result is
    computed and which half is a human claim.
24. **A viewport constant in a repository is not the viewport of the shipped build.** §7.12 took
    DuskOnVita's geometry from `tests/gxm_state_test.cpp: viewport(118,0,724,544)` and from that
    concluded that both ports letterboxed to 738 px. Their v0.9.0 release measures **960 px, the full
    surface** — the test file describes a harness, not the release (§7.16). A number read out of source
    is a claim about one configuration; only the running binary settles what it renders. This is trap 21
    one level down: check where the value is supplied, not where it is named.
25. **A published patch is not an applicable patch.** Celosia's `celosia.patch` declares a base one
    commit past our vitaGL pin, and still fails 3 hunks on that very base and ends mid-line (§7.17).
    Verify a third-party patch by resolving its `index` blob hashes to a real commit, then applying it
    with `--fuzz=0` to a pristine checkout of *your* pin — not by trusting its header, and not by
    applying it to a tree you have already modified (the failures cascade and the count becomes fiction).
26. **A linker flag that exists is not a linker flag that does the job.** GNU ld 2.46 accepts
    `--section-ordering-file` and still cannot order per-function code, because it orders *output*
    sections that must already exist (§7.18). The same idea needs `-Wl,-T,<fragment>` here, while LLD
    uses the first flag with a different file format — so a recipe copied between linkers fails in a way
    that reads like a syntax typo. Confirm the mechanism with two `nm` calls on two links, with and
    without the flag, never by the flag being accepted.
28. **Check WHICH frames your phase split averaged.** A first pass of §7.21 reported decode 3.3 % and
    `other` 59.7 %, and on that basis Rank 2 was written off. The windows had been selected by
    `draws == modal count`, which picked a menu composition, and the real gameplay windows were excluded
    as "huge" outliers. Re-cutting by frame range gave decode 36.9 %, pass 38.4 %, other 8.3 % -- the
    opposite conclusion. A phase split is only as good as its population, so print the frame numbers, the
    draw counts and the window time alongside the percentages, and state which frames were averaged.
    (Borrowing another port's split is a separate hazard, and it happened to be roughly right here.).
27. **An archive is not a set of leaf functions.** `*libc.a:*(.text .text.*)` looks like "put libc
    first" and actually relocates 4,921 functions, most of them stdio/locale/time/filesystem bulk, while
    the two you wanted (`memcpy`, `malloc`) do not move at all. Measured: the hot veneer count went
    58 -> 90 (§7.19b). Archive-level link ordering is too coarse to control distance; if you need to
    control it, go function-granular and *verify the specific symbols' addresses*, not the section
    layout and not the flag being accepted.

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
and frame, the only way to tell whether the §7.4b fixes improved the picture.

**But scope (2) correctly, because §7.16 changed it.** The third port now runs, and it renders **960 px wide**
while ours renders **738**. So it is not a drop-in pixel reference: a same-scene pair differs first by that
resampling, and only what survives a downsample to 738 can be attributed to the ports. Either compare at a
common reduced scale and accept that the comparison is coarse, or treat their frame as a *structural*
reference — is the HUD where it should be, is the minimap drawn (§7.15 says theirs is not) — rather than a
pixel oracle. Stating which of the two is being attempted is part of the job.

**If a run dies:** §7.1's arithmetic is now sane (24 MiB more arena), so watch whether the `0xBF18265F` rate
changes before doing anything else; the failure path already records `caller=%p`.

**The honest position as of this revision:** the port boots, reaches the main loop, renders correctly shaped
and correctly coloured frames, and reaches gameplay in an emulator. A separate port does the same on real
hardware at 12–28 FPS with working saves (§7.5), so feasibility is settled. Still unproven here: save reload,
performance, audio, and all hardware behaviour.
