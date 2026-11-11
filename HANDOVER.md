# Handover: Twilight Princess on PS Vita

**Written:** 2026-10-29
**Updated:** 2026-11-02 (second revision) — the DuskOnVita audit (§7.5) changed two conclusions.
**§7.1 is now partly solved:** the retail GameCube arena trim was live and was removing 24 MiB from
the guest arena on every run; it is now guarded, measured at 40 MiB before and 64 MiB after. The
`0xBF18265F` allocation itself is still unexplained. **§7.2 is downgraded:** the null dereference is
real and fixed, but DuskOnVita ships byte-identical code and boots, so it is latent and is not
established as our crash cause. §7.3 (texture upload) stands and was independently confirmed by that
port. Also added: `NOTICE` and `LICENSES/`, and an honest statement of the vitaGL LGPL position,
which currently makes this repository source-only. The next highest value item is still a reference
desktop render (§7.4).
**For:** a fresh ZooCode session picking up this port
**Read this first, then `PLAN.md` and `README.md`.**

This document records state, verified facts, traps, and open work. Where something is a
guess it is labelled a guess. Where something was measured, the measurement is included so
the next session does not have to repeat it.

---

## 1. What this project is

The Legend of Zelda: Twilight Princess (GameCube, USA/Japan/PAL) ported to the Sony PS Vita,
built as a VPK homebrew package.

It is **not** a from-scratch reimplementation. The stack is:

| Layer | Source | Notes |
|---|---|---|
| Game code | `dusklight` (a decompilation of the GC game) | submodule-style checkout in `dusklight/`, at `77705f8bb95ef7b7dca4675b149c4bcb6a48da34` |
| Renderer frontend | `aurora` (Dusklight's GX→WebGPU backend) | nested at `dusklight/extern/aurora`, pinned by dusklight to tag `pre-upgrade-3227d76` |
| GLES2 translation | `vitaGL` | supplied by VitaSDK |
| Platform | Sony Vita SDK (`arm-vita-eabi`, GCC 15.2.0), SDL3 3.4 | `VITASDK=/home/yanserm1/vitasdk` |
| Vita glue | `tp-vita/` | **this is the only code the port actually owns** |

`tp-vita/` is **not a git repository**. There is no local version control for the port's own
work. `dusklight/` *is* a git repo and carries port modifications as uncommitted changes.

### The merged-tree architecture (important)

`tp-vita/build/aurora_lib` is generated at configure time. It is *not* hand-edited and *not*
a checkout. It is assembled by `tp-vita/tools/build_aurora_tree.py` from three inputs:

1. `dusklight/extern/aurora/lib/**` — upstream, unmodified
2. `tp-vita/vita/aurora/overlay/**` — **whole-file replacements** (same relative path wins)
3. `tp-vita/vita/aurora/patches.py` — **exact-once string substitutions**, applied after the overlay

Consequences:

- Never edit anything under `build/aurora_lib` directly; it is regenerated and your edit is lost.
- To change a file, either copy it into `overlay/` or add a line to `patches.py`.
- Each `patches.py` entry must match **exactly once**. If the anchor text is not found, the build
  script fails loudly. This is deliberate and it is what keeps patches honest.
- Overlay files are full replacements, so when upstream changes, the overlay must be re-reviewed
  by hand. There is no merge.

---

## 2. Current status: honestly

**The game boots, reaches its main loop, renders, and on a driven run reaches real gameplay.**
That last part is new (2026-11-09) and is the strongest result in this document: see the bullet below and
§3d.6.

Verified as of this handover:

- Build is clean: `cmake --build build -j` exits 0, produces `build/tpv_vita.vpk`,
  text→data gap 105,200 bytes.
- `mDoMch_Create()` returns 1 (previously it never returned).
- `main01()` enters its `do { ... } while (dusk::IsRunning)` loop and keeps iterating.
- Vita3K renders real frames: after the §7.3 texture fix, screenshots carry 78,360–87,870 distinct
  colours each (before it: 1,614–3,979, i.e. untextured), with content
  bounding box `(271,128)–(1008,671)` on a 1280×800 canvas.
- **A driven run reaches actual gameplay and the picture was validated by eye** (2026-11-09, PAL disc
  `GZ2P01.iso`). Run `pr_`, frames 2,150–4,800, 16 valid screenshots: Link's lit, textured face with a
  working dialogue box; two characters with the HUD; and an outdoor scene with distance fog, terrain, a
  riverbank, foliage and a mounted rider. Consecutive frames differ by 34–39% of pixels, so it is a live
  scene, and draw counts hold at 215–376/frame. This is the artefact §11 previously wanted a desktop
  reference render for; the user judged it from the screenshots instead.
- Zero failing allocations, zero crash markers, zero `GL error uploading` warnings during a clean
  run, and `JKRExpHeap::create` succeeds repeatedly with ~21.9 MiB free in `gameHeap`.
- The guest arena is now **64 MiB** (`67092240` bytes). Until this revision the retail GameCube
  trim was silently removing 24 MiB from it every run (§7.1).
- All host test suites pass (numbers in §6).
- The vertex decode path is **measured separately from the frame** at 3.95–6.45 ms/frame in Vita3K
  (§7.9), which is the number to compare against when optimising it. A content-addressed decode cache
  exists, is correct, hits 74%, and is **off** because that is too small a share here to measure.
- A fatal path now **names itself, correctly**: `abort()` is intercepted link-time and every death
  writes `ux0:data/tpvita/crash.log` with the caller resolved to a symbol (§7.10, §7.11). The first
  version named the *wrong* function (43.8% of symbols were missing from its table, and every size was
  0); the table now comes from `nm -S -C` on the shipped ELF, rides in the VPK, and carries an anchor
  address that is checked on **every boot** and again at crash time. Proven by a deliberate abort,
  which recorded `abort_caller=0x81009bab main`, verified against `nm` to be inside `main`.

**What that does not prove.** Vita3K is an emulator; nothing here has run on real hardware.
Audio is disabled (`DUSK_AUDIO_DISABLED`). Card saves, pause menu, and most of the game's content have
never been exercised. Treat "boots and renders" as the accurate claim, not "playable".

**Scene correctness now has two independent checks.** (1) 2026-11-05: against a reference frame of the
retail title screen supplied by the user, after the §3d.3 render-scale fix the captured title screen
matches — logo at the correct size and position, the wolf on the ridge, the full composition — where
before it was magnified and cut off below the logo. That is a real check of the 2D/EFB path (scale,
aspect, letterbox). (2) 2026-11-09: the **in-game** screenshots above were judged correct by the user by
eye, which is what §7.4's desktop reference render was for. Neither comparison is a per-pixel diff, so
treat them as "the picture looks right", not "every 3D effect is proven" — most of the game's content is
still unseen.

---

## 3. Environment and commands

Set these in every shell:

```sh
export VITASDK=/home/yanserm1/vitasdk
export PATH="$VITASDK/bin:$PATH"
```

### Build the VPK

```sh
cd /home/yanserm1/psvita/tp-vita
cmake -S . -B build            # ALWAYS, see the trap below
cmake --build build --target tpv_vita-release.vpk-vpk -j"$(nproc)"
```

**Two traps, both silent, both cost time in this session.**

1. **The target is `tpv_vita-release.vpk-vpk`, not `tpv_vita-release.vpk`.** Ask for the wrong name and
   `cmake --build` prints nothing, exits **0**, and builds nothing; check `ls -la build/*.vpk` against the
   clock rather than trusting the exit code (`cmake --build build --target help | grep vpk` lists the real
   one).
2. **The aurora overlay is only re-synced at configure time.** Editing `vita/aurora/overlay/**` or
   `vita/aurora/patches.py` and then building regenerates nothing, so the build silently uses the previous
   tree. This bit me here: after restoring a reverted fix in the overlay, the suites still failed, because
   `build/aurora_lib/` still held the reverted copy. Always run `cmake -S . -B build` after touching either,
   and verify with `grep` in `build/aurora_lib/` that the change actually landed.

Fresh configure (only needed if CMake files changed):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTPV_PACKAGE=ON
```

### Run in Vita3K (headless, in podman)

```sh
cd /home/yanserm1/psvita/tp-vita
VITA3K_DIR=/home/yanserm1/vita3k-linux \
TPV_V3K_WORK=/home/yanserm1/vita3k-work \
./tests/vita3k/run.sh run <interval_seconds> <num_screenshots>
```

`run.sh` is a wrapper around a container. Practical notes:

- **The first N seconds of any run produce blank frames.** A run of `40 2` gives two black PNGs;
  `75 4` gives four real frames. Do not conclude "rendering is broken" from a short run.
- Screenshots land in `$TPV_V3K_WORK/out/shot_N.png`, log at `$TPV_V3K_WORK/out/vita3k.log`.
- The guest trace file is at
  `$TPV_V3K_WORK/home/.local/share/Vita3K/Vita3K/ux0/data/tpvita/dvd_trace.txt`.
  **Delete it before each run** or you cannot tell old lines from new.
- A blank screenshot is `mode=1` (1-bit) and 263 bytes. A rendered one is `mode=RGB` and several
  KB. Use that as a cheap first check.

### Analyse screenshots without vision

The model cannot see images. Decode them numerically:

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

PIL 12.2.0 and numpy 2.4.6 are available. Expected healthy output is `mode=RGB`,
`colours` in the thousands, `bbox=(271, 128, 1008, 671)`.

That bbox is not arbitrary: the EFB is 608×448, scaled by 1.2143 to 738×544, placed with
`glViewport((960-738)/2, (544-544)/2, 738, 544)` = `(111, 0)`, and the Vita surface is centred 1:1
inside the 1280×800 emulator window at `(160, 128)`. Hence `160+111=271`, `128+0=128`,
`271+738-1=1008`, `128+544-1=671`. **This is pixel-exact and was verified by measurement.** If a
future run disagrees, something in `present()` changed.

---

### 3b. Build variants: what ships vs what you debug with (2026-11-03)

Two deliverable builds, selectable with one command. Before this, `CMAKE_BUILD_TYPE` was nearly inert
and `-g` was unconditional, so "Release" was not a considered release and there was no dev build.

```
tools/build.sh release   -> build/tpv_vita-release.vpk      -O3, NDEBUG,   title TPLG00001
tools/build.sh dev       -> build-dev/tpv_vita-dev.vpk      -O1, asserts,  title TPLG10001
tools/build.sh debug     -> build-debug/tpv_vita-debug.vpk  -O0, asserts,  title TPLG20001
```

`dev` is the one to reproduce a failure with. Not `-O0`: this runs in an emulator that is already slow,
and an unoptimised build can fall below the frame rate where an intermittent bug appears, which defeats
the purpose. `-O1` keeps frame pointers and stays close to the shipped code.

#### The one invariant: struct layout must not differ

CMake's stock Debug flags inject `-DDEBUG`, and dusklight adds members under `#if DEBUG`
(`d_meter_HIO.h`, `f_ap_game.h`, `d_map.h`, ...). That changes `sizeof()` of game objects, so a dev
build would be a *different program* from the one that ships and a reproduced bug might not be the
shipped bug. `-DDEBUG`/`-D_DEBUG` are therefore stripped in every variant and `DEBUG` is pinned to `0`,
verified by preprocessing `dusk_pch.hpp` with the dev build's exact flags: `debug_is_on = 0`.
`PARTIAL_DEBUG=1` is the safe half of that instrumentation and is already on everywhere.

#### Three traps found while wiring this, all silent

1. **The toolchain owns the build-type flags.** `vita.toolchain.cmake` sets
   `CMAKE_CXX_FLAGS_DEBUG="-O0 -g -DDEBUG -D_DEBUG"`, and build-type flags are appended *after*
   `add_compile_options`, so a variant flag added as a compile option loses. That is also where the
   unwanted `-DDEBUG` came from.
2. **A custom build type drops every flag.** CMake reads `CMAKE_CXX_FLAGS_<UPPERCASE BUILD TYPE>`, so
   `CMAKE_BUILD_TYPE=Dev` reads an all-empty `CMAKE_CXX_FLAGS_DEV`. Measured: the cache held the right
   value in `CMAKE_CXX_FLAGS_DEBUG` while the generated compile line had no `-O` flag at all.
3. **`add_compile_options` does not split on spaces.** `"-O1 -fno-omit-frame-pointer"` reached the
   compiler as ONE argument, giving `cc1plus: error: argument to '-O' should be a non-negative
   integer`. `separate_arguments` is required.

Resolution: build-type flags are cleared to empty (removing the toolchain defaults and `-DDEBUG`) and
one `add_compile_options` list is the only authority. One place to read, no ordering question.

#### Smaller choices worth keeping

- **Distinct title IDs and VPK names.** Both builds used to be `TPLG00001` / `tpv_vita.vpk`, so
  installing the dev build overwrote the release one and a screenshot or frame count could not be
  attributed to a build. The ID must be 4 uppercase letters + 5 digits (`check_vpk.py` enforces it),
  so the variant goes in the digit field: `00001` / `10001` / `20001`.
- **`boot.log` states its own variant** (`BUILD: dev (asserts ON, ...)`), so a pasted log is
  self-describing.
- **The Vita3K harness prefers the dev VPK** when present; `TPV_VPK=` overrides.

### 3c. The intermittent death is a borealis FATAL — proven, not inferred (2026-11-03)

The dev build reproduced the crash on its first run and the §7.11 recorder named it:

```
=== abort() from abort() ===
abort_caller=0x81a8fbbb  borealis::log::detail::write_fatal(std::basic_string_view<char, ...>, ...)
symbols: table matches this build
```

So the earlier guesses — `PPCHalt`, `OSSwitchFiber`, a config double-registration — were wrong for this
crash: it is a `LogLevel::Fatal` raised through borealis. This is the first time the project has known
*what kind* of death it is, and it came from the fixed recorder rather than from reading logs.

What is still missing is the **message**, which is why [`dusklight/src/dusk/logging.cpp`](../dusklight/src/dusk/logging.cpp)
now installs `Options::onFatal`: it writes to stderr (which lands in `vita3k.log`) and to
`ux0:data/tpvita/fatal.txt`. The hook is necessary because of a real gap in borealis: when the logger is
not initialised a fatal goes to `write_bootstrap_locked`, which pushes onto `state.bootstrap`, and that
queue is replayed **only inside `init()`** — so a fatal after init began but before it completed is
never written anywhere. `onFatal` is called in both branches, before `std::abort()`.

**Correction to the paragraph that used to be here.** It claimed "six subsequent dev runs did not
reproduce it" and that the death might be "sensitive to the optimised build". Both claims were wrong:
the dev build is where the `write_fatal` frame came *from*, and a `frame=9` run was a short harness
window, not a death. Recorded here because I wrote it down as fact at the time.

**14 consecutive release runs then produced no fatal at all** — 8 at 60 s, 3 at 150 s, and 3 at
240 s × 2 shots that reached frame 7,200–7,500 — with no `crash.log`, no `fatal.txt`, and no guest
exception in any emulator log. That retires the "roughly 1 run in 7" figure below, which was measured
on an older tree. Either one of the intervening changes fixed it (§7.1's arena fix is the plausible
candidate: it removed a 24 MiB shortfall that made allocation failure fatal on `TARGET_PC`), or its
rate is simply low enough that 14 runs establish nothing. **Both readings fit the data and neither is
proven.**

### 3d. The idle runs never left the attract loop — and the game stops at a memory-card prompt

The negative result above has a cause, and it is a measurement error of exactly the kind §7.8 collects.

Idle runs do not play the game. The twilight-sky frames they produce are the **title / attract loop**,
not gameplay. An idle run therefore never loads new scene data, and the data-dependent fatal sites
cannot fire:

- [`gx.cpp:161`](vita/aurora/overlay/gx/gx.cpp:161) calls `comp_cnt_count()`, which `fatal()`s on an
  unsupported `GXAttr`/`GXCompCnt` pair;
- it is reached per display list from [`dl.cpp:62`](../dusklight/build/aurora_lib/gx/dl.cpp:62) and
  [`command_processor.cpp:366`](../dusklight/build/aurora_lib/gx/command_processor.cpp:366).

New geometry with new vertex attribute formats is what moving through the game produces. 7,500 idle
frames on one screen cannot produce it. So "14 clean runs" is weaker evidence than it looks.

To test this properly, [`tests/vita3k/in_container_script.sh`](tests/vita3k/in_container_script.sh)
replays a **fixed action script** (grammar in that file; scripts live in `tests/vita3k/scripts/`), and
`run.sh script <file> [prefix]` mounts and runs it. Its header states the honest limit plainly: the
**key sequence is fixed, the timing is not** — presses are wall-clock, so the emulator's variable speed
means a press can land on a different frame each run. That is a reproducible sequence, not a
reproducible run. A frame-counted injector would need a hook inside the app, not an `xdotool` driver.

**Input does reach the game.** A tap moved the screen from "No Memory Card found in Slot A" to
"Continue without saving?" with the cursor shown, so the SDL3 → `aurora::input` → `PADRead` path works.

**But the game cannot get past it.** Every route returns to the same check:

| Step | Code | Result |
|---|---|---|
| status read | [`mDoMemCd_Ctrl_c::getStatus`](../dusklight/src/m_Do/m_Do_MemCard.cpp:388) | `CARD_STATE_NO_CARD_e` → `0` |
| `case 0` | [`dFile_select_c::MemCardStatCheck`](../dusklight/src/d/d_file_select.cpp:4934) | `ERRMSG_WAIT_KEY` then `NO_SAVE_SEL_DISP` |
| after the prompt | `MemCardStatCheck` again | `0` again → the same dialog |

So it is a **self-perpetuating loop**: answering "Continue without saving?" re-runs the check that
fails, and A-pressing never opens the game. The "Checking Memory Card in Slot A…" header is permanent
chrome on that screen, not a transient status — which is why it is still there minutes later.

One hypothesis was tested and **ruled out**: that the card file could not be created because its parent
was the disc image rather than a directory (an earlier Vita3K log showed `.../GZ2P01.iso/EUR/Card A`).
`VitaUserPath` is `ux0:data/tpvita/` ([`backend.cpp:58`](vita/aurora/backend.cpp:58)), and creating
`ux0:data/tpvita/EUR/Card A` by hand **did not change the state**: same shots, same md5, and the
directory was still empty afterwards. The parent-path theory is dead.

What is established, and what is not:

- **Established:** status is `0`; the loop is self-perpetuating; input works; creating the card
  directory by hand does not help; `CARDMount` always returns `CARD_RESULT_READY`
  ([`card.cpp:664`](../dusklight/extern/aurora/lib/dolphin/card.cpp:664)), so "no card" is decided by
  `CARDCheck` → `card->getError()`, not by the mount.
- **Not established:** *why* the GCI-folder channel reports `NOCARD` on this port — **answered in
  §3d.1 below.**

### 3d.1 The cause was a dangling path, not card logic (2026-11-05)

The plan above ("print `mCardState`") was right about the location and wrong about the mechanism. The
card code is fine; it was being handed a corrupted base path.

**Root cause.** `AuroraConfig`'s path strings are borrowed pointers that upstream copies with `strdup()`
([`extern/aurora/lib/aurora.cpp`](../dusklight/extern/aurora/lib/aurora.cpp)). The Vita backend
([`vita/aurora/backend.cpp`](vita/aurora/backend.cpp)) stored them raw. Dusklight builds them from
`std::string` temporaries inside a block of `game_main()`
([`m_Do_main.cpp`](../dusklight/src/m_Do/m_Do_main.cpp)); the block ends and the buffers are freed and
reused. `g_config.userPath` was read later, by `CARDInit()`, and had by then become the **disc image
path** (`ux0:data/tpvita/GZ2P01.iso`), so the card folder resolved to
`ux0:data/tpvita/GZ2P01.iso/EUR/Card A`. Creating a directory under a *regular file* fails with ENOENT,
`CardGciFolder::open()` left `m_error = NOCARD`, `CARDCheck()` reported `NOCARD`, and
`dFile_select_c::MemCardStatCheck` looped forever — the §3d symptom, arriving from a place nowhere near
the card code.

The evidence is the emulator log, not an inference: before the fix it read
`Missing file at .../tpvita/GZ2P01.iso/EUR/Card A`; after it, `.../TwilitRealm/Dusklight/EUR/Card A`.

**Fix.** `copy_string()` in [`vita/aurora/backend.cpp`](vita/aurora/backend.cpp) takes owned copies into
static storage (upstream's lifetime, without leaking), refuses a path over 512 bytes rather than
truncating it silently into a *different* directory, and logs the resolved paths on every boot:

```
paths: user 'ux0:/data/TwilitRealm/Dusklight/', cache '...', resources 'app0:/', app 'Dusklight'
```

**A second, independent cause, also found by measurement.** Even with the path fixed, the card directory
was still never created: `_sceIoMkdir` on `.../Dusklight/EUR/Card A` returned `0x80010002` (ENOENT)
because the parent `EUR` did not exist, and creating `EUR` from the host made the path valid.
`aurora::io::create_directories()` calls `SDL_CreateDirectory()`, which is *documented* as creating
missing parents — but that is a property of each platform implementation. Disassembling the shipped
library settles it: `SDL_SYS_CreateDirectory` in `SDL_sysfsops.c.obj` is a bare `mkdir` (compare
`SDL_SYS_GetPrefPath` in `SDL_sysfilesystem.c.obj`, which does walk and create each component). On the
Vita the header's promise is simply not kept.

`create_directories()` now builds the path component by component itself, and **skips a component ending
in `:`** — `SDL_CreateDirectory("ux0:")` fails, and on a Vita it cannot even be stat'd (`Failed to
translate path: ux0:` in the emulator log), which turned the whole call into a failure on its first
iteration. That device-root skip is required, not an optimisation.

**Verified in Vita3K (2026-11-05).** With `ux0:data/TwilitRealm/Dusklight/EUR` deleted first, a
completed run leaves:

```
TwilitRealm/Dusklight/EUR/Card A     (directory, created by the game)
```

so `CARDInit` now formats the GCI folder and the card channel comes up. This is the first time the port
has had a live memory card. **Saves are still untested** — a written save, a re-read and a reload have
not been exercised, and nothing here is verified on hardware.

**How this was nearly shipped broken, and the rule it restates.** The first attempt edited
`dusklight/extern/aurora/lib/io.cpp` directly. That file is upstream source: the build compiles
`tp-vita/build/aurora_lib`, which `tools/build_aurora_tree.py` assembles from upstream + overlay +
`patches.py` on every configure. The edit therefore compiled cleanly, produced a VPK, and changed
nothing — two emulator runs were spent concluding "the fix did not work" when it had never been
included. The change now lives in `vita/aurora/patches.py` as an exact-once anchor against upstream
`io.cpp`, and the upstream file is restored. This is the rule already written down in §8 ("never edit
`build/aurora_lib`"); the missing half is that editing *upstream* `extern/aurora/lib` is equally
ineffective, and only `overlay/` or `patches.py` reach the build.

### 3d.4 What the 2026-11-05 changes were tested against, and what was not

Run (`./tests/host/<suite>/run.sh`):

| Suite | Result |
|---|---|
| `crash` | PASS (7 checks) |
| `texture` | 3,780 checks, 0 failures (the one MISMATCH printed is the deliberate negative control) |
| `disc` | `test_probe` 14/0, `test_diag` 10,686/0 |
| `vertex` | **71,063 checks, 0 failures** (after the fix below) |
| `e2e` | **87 scenes, 14,242 pixel checks, 0 scenes failed** (after the fix below) |

Both suites were **broken before this session and are fixed now**, and both breaks were the kind that
silently removes a test rather than failing loudly:

- `vertex` could not link: `undefined reference to aurora::log_internal`, and then
  `aurora::g_config`. The decoder reports through `aurora::Module`, which needs `logging.cpp` plus a
  `g_config` definition; neither was on the link line. Fixed by adding `logging.cpp` and
  [`tests/host/vertex/stubs.cpp`](tests/host/vertex/stubs.cpp) to `run.sh`.
- `e2e` could not compile: `SDL3/SDL_events.h: No such file or directory`. The header **was** available
  the whole time — in `$VITASDK/arm-vita-eabi/include/SDL3` — but the header-symlink list in
  `tests/host/build.sh` and `tests/host/e2e/run.sh` named only `GLES2 KHR EGL fmt`, and the merged tree
  reaches `SDL3/SDL_events.h` through `aurora/event.h` (`window.hpp` → `gx.cpp`, `GXFrameBuffer.cpp`).
  Adding `SDL3` to both lists fixed it. The earlier reading of this, "this machine has no SDL3", was
  wrong and is recorded here because it was written down as fact.

**The e2e suite had in fact been red since the 75 % render scale was introduced (§7.7).** Measured, not
assumed: with this session's four viewport/scale files stashed, `e2e` reports **84 of 87 scenes
failed**; with them applied, 30; after the scale fix below, **0**. Nothing had been run in between, so
the suite which exists to catch exactly this class of error was not consulted when the error was added.

The remaining 30 were one cause: the suite asserts exact pixels (a 1-px line, an 8-px point, a scissor
edge, a TEV colour), and at 75 % those are resolved by filtering — a full-intensity line came back as
`(15,210,15)` instead of `(0,255,0)`, i.e. ~0.588 of it. Rendering the host suite at 100 % and keeping
75 % on the Vita removes the mismatch at its source instead of loosening the tolerance, which would have
hidden real regressions too. `kEfbScalePercent` is therefore `75` on `__vita__` and `100` elsewhere;
the device log still reads `EFB target: logical 608x448 -> 456x336 (75%)` with viewport `456.0x336.0`,
so the scaled path is the one hardware runs and the unscaled one is the reference it is checked against.

Not verified at all, on any target: a save actually written and reloaded; audio; performance; anything
on real hardware.

### 3d.2 The memory-card prompt no longer blocks play (2026-11-05)

`shot_5` of the scripted run is an in-game scene (a lit stone bridge over a dark cavern): the game got
past file select and into gameplay while the card channel still reported `NOCARD`. So the earlier
ranking — "the port cannot be played past file select at all" — was a consequence of the scale bug in
§3d.3, which drew the dialog far off-screen, not of the card state.

**Correction (2026-11-05, §3d.6): the sentence above overstates what that shot showed.** "A lit stone
bridge over a dark cavern" was a reading of a picture nobody had compared to anything, on a port whose
own §7.4 says no reference render exists. What is defensible from the artefacts is narrower: the card
prompt was answered, a GCI was written, and the game advanced **into the save-creation sequence** —
which is where it then stayed, on the name-entry grid, for eleven consecutive screenshots (§3d.6).
Gameplay proper has still never been observed. The lesson is §3d.6's: a filename and a remembered
description are expectations, and only the pixels count.

**Saves work (write path, 2026-11-05, later the same day).** A frame-counted run (§3d.5) answered "Yes"
to "Would you like to create a save file ... in Slot A?" and the game wrote a real GCI:

```
TwilitRealm/Dusklight/EUR/Card A/01-GZ2P-gczelda2.gci   32,832 bytes
header: 47 5a 32 50 30 31 00 05 "gczelda2"      (GZ2P01, 5 blocks, filename field)
10,497 non-zero bytes -- real data, not a zero-filled stub
```

The run then continued into the new-game intro ("Enter horse name", "Epona") and later "Enter name",
"Link", which is further than this port has ever reached. What that still does **not** establish: that
the save RELOADS. A second run with the GCI in place was driven the same way and reached the naming
screens again, i.e. it began a new game rather than continuing the saved one. Whether that is the
intended behaviour for a card that exists but was never *loaded* in this session, or a defect in the
load path, is NOT determined — the run did not capture the file-select screen itself. Treat save
RELOAD as untested.

### 3d.3 Why the title screen was cut, and the rects to measure next time (2026-11-05)

The user reported the title screen was cropped: the logo filled the frame and "PRESS START" plus the
copyright line were missing. That is a **render-scale bug**, not a scene bug, and the frames recorded
the exact numbers.

`ensure_efb()` allocates the EFB at 75% of the logical size (608x448 -> 456x336). But
`gfx::vita::main_target_size()` returned the **logical** size and `get_render_target_size()` returned
`main_target_size()` for the main target, so `gx::map_logical_viewport()` computed
`target / logical` = `608/608` = **1.0**. The 75% was never applied to anything. Every full-screen
608x448 viewport was therefore issued against a 456x336 FBO: the picture was magnified by 1/0.75 and
cropped, which is exactly a title screen losing everything below the logo.

Measured before the fix, from the log:

```
EFB target: logical 608x448 -> 456x336 (75%), present 960x544
rect on 456x336: viewport (0.0,0.0) 608.0x448.0  scissor (0,0) 608x448
```

i.e. a viewport 1.33x the target, and a scissor clamped to 456x336 while the geometry ran to 608x448.

**Fix.** `efb_target_size()` (declared in [`vita_frame.hpp`](vita/aurora/overlay/gfx/vita_frame.hpp),
defined in [`vita_gl_replay.cpp`](vita/aurora/overlay/gfx/vita_gl_replay.cpp)) returns the scaled size,
`get_render_target_size()` reports it for the main target, and `ensure_efb()` allocates from the same
function so the two cannot drift apart. After the fix the log reads:

```
rect on 456x336: viewport (0.0,0.0) 456.0x336.0  scissor (0,0) 456x336
```

and [`shot_3`] now matches the real title screen: logo at the correct size, the wolf on the ridge, and
the layout the reference frame shows.

**Keep the instrumentation.** `apply_viewport()` logs each distinct (target size, viewport, scissor)
once per process, and `ensure_efb()` logs the logical -> EFB conversion. Two lines of log settled in
minutes what several paragraphs of reasoning had guessed wrong, so do not delete them to tidy up.

This also outranked the intermittent `write_fatal`: at the time the port could not be played at all
past file select, which was a hard blocker for every other test. It was invisible for the whole project
because nothing had ever driven the game with input. **That is now solved** — see §3d.5, which drives
the game with frame-counted input and gets as far as the new-game naming screens.

---

### 3d.5 Frame-counted input: `vita/src/input_script.cpp` (2026-11-05)

`tests/vita3k/script` replays keys with `xdotool`, and its own header says the honest limit out loud:
the key SEQUENCE is fixed, the TIMING is wall-clock. The emulator's speed varies run to run, so a press
lands on a different frame each time. That is a reproducible key sequence, not a reproducible RUN.

The injector fixes that by counting frames. It is one new file plus three lines of wiring, and it
needed **no change to dusklight**:

| Piece | Where |
|---|---|
| the script parser and the pad push | [`vita/src/input_script.cpp`](vita/src/input_script.cpp) |
| hooked into the loop, once per frame, before the game reads the pad | [`vita/aurora/backend.cpp`](vita/aurora/backend.cpp) `aurora_update()` |
| loaded at boot, logged loudly | [`vita/src/main.cpp`](vita/src/main.cpp) `main()` |
| image content | `TPV_VITA_SOURCES` in [`CMakeLists.txt`](CMakeLists.txt) |

The hook point matters. `aurora_update()` runs once per main-loop iteration and BEFORE the game's
`JUTGamePad::read()` -> `PADRead()`, so a push made there is what the game sees on that same frame.
Nothing in the game's code had to be touched, which is why this stays mergeable with upstream.

**Format** (`ux0:data/tpvita/input_script.txt`; absent = off, zero cost):

```
# comment
<frames> <name>[,<name>...]     hold for N frames
<frames> .                      hold nothing for N frames
```

Two name sets, and the distinction is the whole lesson of this section:

* **Buttons** (`PADStatus::button`): `A B X Y START LEFT RIGHT UP DOWN L R Z`
* **Stick** (`PADStatus::stickX/stickY`): `STICKLEFT STICKRIGHT STICKUP STICKDOWN`

`LEFT` and `STICKLEFT` are NOT the same input, and Dusklight's menus read the stick, not the D-pad.
[`d_file_select.cpp`](dusklight/src/d/d_file_select.cpp) calls `STControl::checkLeftTrigger()`, which
needs a nonzero `stickValue` and derives direction from `stickAngle`. A script that pressed `LEFT` set
`PAD_BUTTON_LEFT`; nothing on that screen looks at that bit, so the cursor never moved, `A` confirmed
"No" again, and the game looped between "create a save file?" and "continue without saving?". That
loop was reproduced twice before the cause was found.

**Full deflection, and why a smaller push is a trap.** The chain is
`PADClamp()` -> `JUTGamePad::CStick::update()` -> `STControl::checkTrigger()`:

* `PADClamp`'s `ClampStick` subtracts a dead zone of 15 and rescales by `72/(127-15)` for the main
  stick; a raw 100 on one axis comes out as 54, not 100.
* `CStick::update` divides by 54 (`EClampStick`, main stick), so 54 -> `mValue` 1.0 — just over the
  0.9 threshold `dFile_select.cpp` constructs its `STControl` with. A raw 90 becomes 45, 45/54 = 0.83,
  and the cursor never moves while the log looks perfect.
* The divisor is not even fixed: under `EClampCircle` it is 56/38. Full deflection (127) saturates to
  1.0 under every clamp, which is why it is the only value worth using.

**Three defects the first version had, each found by measurement rather than review:**

1. Trailing `#` comments were not stripped, so `60 .   # settle` was rejected as an unknown name and
   the WHOLE script was discarded (correct behaviour for a typo, wrong for a comment).
2. newlib's `vsnprintf` does not support `%zu`. The summary printed a literal `zu` and consumed no
   argument, shifting every later value in the line by one.
3. No stick output at all — the bug described above.

**The one honest limit.** Frame numbers are exact; the SCREEN at a given frame is not. The card prompt
is a ~28-state machine (`MemCardCheckMain`) whose states run variable-length animations, so the same
script can reach the prompt at a different frame. A single precisely-timed press is therefore the
WRONG shape for such a screen — one press landed on a plain message with no cursor and was simply
lost. The scripts in [`tests/vita3k/scripts/`](tests/vita3k/scripts/) therefore use a sweep that
alternates a cursor move with `A`: every branch either advances or re-enters the sweep, so it cannot
get stuck. That is a deliberate design, not sloppiness.

**Where the output goes, and why it is a file.** `[input script]` lines are written to
`ux0:data/tpvita/input_script.log` and to stderr. The file is the part that can be read back: Vita3K
does not forward the guest's stderr into `vita3k.log` — measured, not assumed — and on a real Vita
there is no log to read at all. Every step is logged with the frame it fired on, so a run can be
CHECKED against its script instead of being believed:

```
[input script] frame 1050 -> buttons 0x0000 stick -127,0 for 30 frames (30 STICKLEFT)
```

**Run it:**

```sh
w="$HOME/vita3k-work/home/.local/share/Vita3K/Vita3K/ux0/data/tpvita"
cp tests/vita3k/scripts/frame_card_sweep.txt "$w/input_script.txt"
VITA3K_DIR=... TPV_VPK=build/tpv_vita-release.vpk \
  tests/vita3k/run.sh script tests/vita3k/scripts/waits_sweep.txt sw_
# then read $w/input_script.log
```

Use the `waits_*.txt` harness scripts, not the key-pressing ones: when the app is driven by the
injector, a host key press is noise that can change the result.

**What this proved, in Vita3K only:** the title screen renders correctly and uncropped (§3d.3 fixed);
`START` advances it; the card prompt appears and is answerable; "Yes" creates a real 32,832-byte GCI;
and the game reaches the new-game naming screens. **Not proved:** save reload, audio, performance, or
anything on real hardware.

### 3d.6 The name screen exits correctly; the picture reaches real gameplay (2026-11-05, corrected 2026-11-09)

**Headline, and it is the strongest result in this document: the port renders actual Twilight Princess
gameplay.** Driven run `pr_`, frames 2,150–4,800 of a PAL disc (`GZ2P01.iso`), 16 valid screenshots:
Link's face lit and textured with a dialogue box (`pr_13`), Link and another character with the HUD
(`pr_15`), and an outdoor scene with distance fog, terrain, a riverbank, foliage, a rock outcrop and a
mounted rider with the horse (`pr_16`). Frames change 34–39% pixel-wise between shots, so it is a live
scene, not a still. Draw counts settle at 215–376/frame.

That is a **picture the user validated by eye from the screenshots** (§11: the desktop reference render
is de-scoped and this replaces it). It is not a claim that the rendering is complete or correct in
detail — see the caveats at the end.

#### The name screen was never stuck. My "dead end" diagnosis was wrong, twice.

The screen is `dName_c` with a nine-entry `mSelProc` state machine (`include/d/d_name.h`):

```
proc: 0=MOJI_SELECT 1=MOJI_SEL_ANM 2=MOJI_SEL_ANM2 3=MOJI_SEL_ANM3
      4=MENU_SELECT 5=MENU_SEL_ANM 6=MENU_SEL_ANM2 7=MENU_SEL_ANM3 8=WAIT
menu: 0=HIRA 1=KATA 2=EIGO 3=END
```

A `[name-proc]` probe in `dName_c::_move()` logging every distinct `(proc, menu, row, col)` tuple, plus a
`[name-press]` probe logging whether START/A/B were *delivered*, gave this sequence on a clean run:

```
proc=0 menu=0 ... cursor walking 12->11->...->0, wrapping to 12 ...
proc=0 menu=0 charCol=10
[name-press] START delivered in proc=0 menu=0
proc=2 menu=2      <- START worked: MOJI_SEL_ANM2, mSelMenu set to EIGO (2)
proc=4 menu=2      <- MENU_SELECT, cursor on EIGO ("ABC")
[name-press] A delivered in proc=8 menu=2   <- proc 8 = PROC_WAIT
```

**`proc=8` after A is the proof of success.** The A branch in `MenuSelect()` calls `menuAbtnSelect()`,
which on a PAL disc jumps to `pal_eigo:`, calls `nameCheck()`, and on a non-empty name sets
`mSelProc = PROC_WAIT` and `mIsInputEnd = 2`. `dFile_select_c::nameInput()` then switches on
`isInputEnd()` case 2 and calls `dComIfGs_setPlayerName()` and moves to `DATASELPROC_NAME_INPUT_FADE`.
There is a second, identical screen for the horse name (`nameInput2()`).

So on this PAL disc, `EIGO_OR_END` resolves to `MENU_EIGO` (2), **not** `MENU_END` (3), because
`DUSK_IF_ELSE((isRegionPal() ? MENU_EIGO : MENU_END), MENU_END)`. That means the menu cursor shows `ABC`
already selected and A confirms from there — the highlighted `ABC` in the screenshot was correct all
along, not a bug.

#### The two wrong diagnoses, recorded so they are not repeated

1. **"A types letters, so the screen is a dead end"** (2026-11-05). True that A types in
   `PROC_MOJI_SELECT`, but the screen also moves on: START at `proc=0` switches to `proc=2` and then
   `proc=4`, and A there confirms. The `LinkJF` text in the frozen screenshots was typed by the *sweep*,
   not evidence of a trap.
2. **"START then A is ignored, the screen is stuck at `proc=4`"** (2026-11-09, earlier the same day).
   Wrong: A *was* delivered and did exit, to `proc=8`. The reason the frames looked identical was
   **sampling time, not game state** — `zz_7`–`zz_9` were captured at 6–9 minutes, but the exit happens
   at frame ~2,150–2,430, about 10 minutes in. Three "frozen" screenshots were three shots of the same
   screen taken before it had any reason to change.

**The transferable lesson:** two screenshots taken seconds apart showing the same pixels is evidence
about *when the screenshots were taken*, not about what the game can do. The instrument that settled this
was a state probe, not another look at the picture.

#### The wrong diagnosis, and what the pixels actually showed

§3d.5 said the game "reaches the new-game naming screens", implying progress. It does not get past
them, and the 8,260-frame run recorded exactly why. Its 16 screenshots were named
`g_naming_1 ... o_gameplay_3`, but the names were **hopes, not observations**. Decoded numerically:

| Shot | Pixels changed vs previous | What it was |
|---|---|---|
| `ig_5` → `ig_16` | **0.09–0.28%** | the same screen, eleven times |

A 0.1% delta across eleven shots 20 s apart is a frozen screen. Rendered on screen was the name-entry
alphabet grid with **`L i n k J F`** in the field — and `F` was highlighted. The letters are the
script's `A` presses. `A` on this screen **types the highlighted character**; it does not confirm.

#### The real exit, read from the source

`dName_c` has two states and the script only ever used one:

| Input | State | Effect | Where |
|---|---|---|---|
| `A` | `PROC_MOJI_SELECT` | **types** the highlighted letter | [`d_name.cpp:1211`](../dusklight/src/d/d_name.cpp:1211) → `menuAbtnSelect()` |
| `START` | `PROC_MOJI_SELECT` | moves the **menu cursor** (ABC/abc/END) to END | [`d_name.cpp:395`](../dusklight/src/d/d_name.cpp:395) |
| `A` | `PROC_MENU_SELECT` + END | `nameCheck()` + `menuAbtnSelect()` → `mIsInputEnd = 2`, screen ends | [`d_name.cpp:1211`](../dusklight/src/d/d_name.cpp:1211) |

So the exit is a **two-press pair, START then A**, and neither alone leaves. There are also **two**
name screens (`d_file_select.cpp:1747` player, `:1845` horse), so the pair has to be repeated.
A repeating `A` cadence is a repeating typist: it added five stray characters and then the field was
full, which is precisely the observed `LinkJF` and then a frozen screen.

#### The PLAY-scene phase chain, and why the probe is kept

[`d_d_s_play.cpp`](../dusklight/src/d/d_s_play.cpp:1660) logs one line per distinct
`(phase index, returned cPhs state)` pair from `dScnPly_Create`, reading `field_0x1c4.id` **before** the
handler runs, so a phase that is never entered cannot be confused with one that succeeds. On a good run
the whole chain is five transitions, all resolved:

```
index=0 -> state=0   INIT/retry   phase_00  (resetGame)
index=2 -> state=0   INIT/retry   phase_1_0 (syncStageRes "Stg_00")
index=5 -> state=0   INIT/retry   phase_2   (object res)
index=6 -> state=0   INIT/retry   phase_3
index=6 -> state=4   COMPLEATE    phase_3 finished
```

Every retry resolved; nothing sat in `cPhs_INIT_e`. That is the same trace seen in the run that produced
the gameplay screenshots above, so **the play scene does run**, and §11's picture claim rests on it.

**Keep the probe.** It is ~15 lines, caps itself at one line per `(phase, state)` pair, and it is the
only thing in the tree that answers "which create phase is the play scene in" without a debugger.

#### What is still not established, and one earlier claim that was wrong

One earlier run *did* show a collapse that this document previously described as the play scene never
starting: draws `443 -> 1` at window 1920, `sResolveSeen` frozen at 19,870, and a wall time pinned to
33.33 ms/frame (the §7.8-trap-3 signature of a game clock pacing a loop with no work). **That blank has
not reproduced** in any run since, and no change was made to fix it. Do not write "the play-scene stall
is fixed" — nothing was fixed, and the honest status is **not reproducible**, the same shape as §7.1's
intermittent that vanished without being understood.

Two real things that collapse did establish, both still true and both worth having:

- **Not a crash.** The main loop ran to frame 8,990 at a steady 30 Hz, `boot.log` was complete, and the
  injector logged a clean finish.
- **Not `ret=4`.** `4` is `cPhs_COMPLEATE_e` — success. On `TARGET_PC`
  [`cDyl_LinkASync`](../dusklight/src/c/c_dylink.cpp:920) returns it unconditionally anyway, so that
  `[DIAG]` line is noise on this build.

If the blank appears again, the `[name-proc]`/`[play-phase]` lines name the last state on the first run.

---

## 4. What was done this session (chronological)

### 4.1 Aurora upgrade attempt, abandoned and fully reverted

The session began from a standing instruction to check upstream `aurora`/`dusklight` for new
commits and pull what was relevant. The plan chosen was: snapshot the working state, re-vendor
aurora to `505b5eb`, fix `patches.py` and CMake, rebuild.

That attempt exposed a hard blocker and was then reverted on instruction. This is worth
understanding because the trap will recur.

**The blocker:** aurora `505b5eb` **removes 22 public PAD functions** from
`include/dolphin/pad.h` in favour of a new `aurora::input` binding system. Dusklight's code uses
the old API in 8 files (`ui/controller_config.{cpp,hpp}`, `ui/input.cpp`, `menu_pointer.cpp`,
`ui/overlay.cpp`, `ui/string_button.cpp`, `ui/command_console.cpp`, `m_Do_main.cpp`,
`d/d_drawlist.cpp`) across 63 usage lines.

**The trap:** it is tempting to conclude "dusklight is behind, nothing to pull". That is wrong.
`git ls-tree HEAD extern/aurora` in dusklight returns
`3227d76c60e1e782ca576610bce61c9e7744d8be` — **dusklight pins `3227d76` explicitly**. Upstream
aurora did not "move on" without dusklight; dusklight has not migrated to the new API yet.
**No upstream branch migrates.** So there is nothing to pull. Do not re-open this without
checking whether dusklight itself has migrated first.

**Also important:** upstream aurora rewrote its history, so `3227d76` is an orphaned snapshot
(`8e356df`'s parent `1acba42` is absent from the object database). Tree-diffs against it
conflate the entire divergence and are misleading. Do not trust a raw `git diff` across that
boundary.

### 4.2 The revert, proven rather than assumed

Since `tp-vita/` has no version control, the revert could not be done with `git checkout`. It was
done by reconstructing and then **byte-comparing**:

1. `git checkout pre-upgrade-3227d76` in the aurora submodule.
2. Extracted the pre-upgrade merged tree to `/tmp/snapcheck/aurora_lib` from the snapshot at
   `.snapshot-pre-aurora-upgrade-20261029/aurora_lib-merged.tar.gz`.
3. Diffed snapshot against upstream `3227d76 lib/`: 13 modified files, 12 overlay-only files.
   Read every diff to recover the exact patch text.
4. Rewrote `vita/aurora/patches.py` from those diffs.
5. Deleted `vita/aurora/overlay/gfx/readback/` (a 505b5eb-only subsystem).
6. Restored `vita/aurora/overlay/gx/texture.cpp` to the 664-line pre-upgrade version.
7. Restored `cmake/aurora.cmake`, `cmake/aurora_gx.cmake`, `vita/aurora/backend.cpp`.
8. Assembled the tree and byte-compared to the snapshot.

**Result: PASS — `patches.py` + overlay reproduce the known-good merged tree byte-for-byte**
(178 files). Three cosmetic mismatches were found and fixed along the way: a dropped
`m_cv.notify_all()`, `std::lock_guard lk{m_mutex}` vs `lk(m_mutex)`, and one lost indentation
level introduced by my own edit.

This byte-identity check is the strongest guarantee available given the missing version control.
**Reuse this technique** for any future patch-set change: rebuild the tree, extract the
known-good snapshot, `diff -rq`. Anything unexpected means you broke a patch.

### 4.3 Cherry-pick audit of the 7 aurora commits

All seven were evaluated individually. **All: Skip.** Summary so this is not redone:

| Commit | What it is | Verdict |
|---|---|---|
| `8e356df` | `GXPeekZ` | Skip — already exists at `3227d76` in `GXCpu2Efb.cpp` |
| `afd4bb8` | `GDLoadTexObj` / `GDLoadTlut` | Skip — nothing we compile calls them; the game uses `J3DGDLoadTlut` |
| `f8347d9` | FIFO processing-mode ergonomics | Skip — our 1-line `patches.py` substitution (`ProcessingMode::Thread` → `Drain`) achieves the same |
| `12943d1` | WebGPU-only change | Skip — irrelevant to a GLES2 backend |
| `d0ace68` | The PAD API removal | Skip — this is the breaking change (see §4.1) |
| `7c8c611`, `505b5eb` | Depend on `d0ace68` | Skip — cannot be taken without it |

### 4.4 Bring-up: two heap bugs found and fixed

These are the substantive fixes of the session. Both were found by measurement, not by reading.

**Bug 1 — Root heap short by 480 bytes, causing `SIGABRT`.**

Symptom: the guest died a few statements before `dvdHeap created` in the bring-up trace.

Diagnosis chain, and the evidence at each step:

1. Symbolised the fault PC → `_kill_r` at `0x82279ccc` with **r2 = 6** (SIGABRT). This proves
   `abort()`, not a null dereference. Do this check first; it saves hours.
2. Traced into `JKRAllocFromHeap` → `JKRExpHeap::do_alloc`.
3. Read the `TARGET_PC` error path: `if (!ptr) { ... CRASH("Aborting due to allocation failure!"); }`
   and `CRASH` = `OSPanic` → `Log.fatal`, which is `[[noreturn]]` → `abort()`. **On `TARGET_PC` a
   failed allocation is fatal, not recoverable.** This is why a 480-byte shortfall killed the process.
4. Instrumented `do_alloc` and measured:
   `do_alloc: size=0x10000 align=16 heap=Root free=0xFE20` — 65536 requested, 65056 free.

Root cause: `arenaSize` (the system heap) is created as a **child** of the root heap, not as the
root heap itself, so it cannot be given the whole arena. The root heap must still cover
`JKRCreateExpHeap(0x10000, NULL)` (the dvdHeap) plus `JKRExpHeap`/`CMemBlock` overhead. No reserve
was budgeted for that.

Fix, in `dusklight/src/m_Do/m_Do_machine.cpp`:

```cpp
constexpr u32 VitaRootReserve = 0x30000;   // 192 KiB, rounded up generously

u32 reserved = archiveHeapSize + j2dHeapSize + gameHeapSize + commandHeapSize +
               dbPrintHeapSize + VitaRootReserve;
```

Rounded generously, not computed exactly, because `sizeof(JKRExpHeap)` varies with compiler and
the failure mode is a hard abort.

**Bug 2 — game heap exhausted at 17 MiB.**

With bug 1 fixed, `mDoMch_Create()` returned 1 for the first time, then hit
`do_alloc: size=0x9AF30 heap=gameHeap free=0x0`.

Measured rather than scaled: the game heap took 57 allocations totalling 167 MiB, largest
`0x108DD20` (16.55 MiB), while the System/zelda pool still held ~8 MiB idle.

Fix, same file:

```cpp
archiveHeapSize = 0x8E0000;  // 8.875 MiB — the un-doubled GameCube value
j2dHeapSize = 0x7D000;       // 500 KiB — the un-doubled GameCube value
gameHeapSize = 0x1700000;    // 23 MiB, taking 6 MiB from the idle System/zelda pool
```

Rationale recorded in the code: the desktop build multiplies by 20 (~85 MiB) which cannot exist
in a ≈40 MiB arena, so the size is set from measurement. This is a 32-bit build like the GameCube,
so the "×2 for 64-bit pointers" adjustment does not apply.

Result: 0 failing allocations, main loop reached, frames rendered.

**Bug 3 — `JKRExpHeap::create` computed `dataPtr` before checking the allocation.**

`dusklight/libs/JSystem/src/JKernel/JKRExpHeap.cpp`: `dataPtr` was computed from `memory` before
the `if (!memory) return NULL;` check. That is undefined behaviour on a failed allocation, and GCC
can fold it. Reordered so `dataPtr` is computed after the null check, with a `// NOTE:` comment
explaining why the order matters. Behaviour is otherwise identical.

### 4.5 Trace instrumentation: what is kept and what was deleted

A large `fopen`/`fprintf`/`fclose`-per-call trace exists because a guest abort discards buffered
stdio and `OSReport` output is lost when the process dies.

**Kept** (one-shot boot staging, cheap, still useful for the open BMD investigation):

| Helper | File | Sites |
|---|---|---|
| `mch_trace` | `dusklight/src/m_Do/m_Do_machine.cpp` | 40 |
| `m01` | `dusklight/src/m_Do/m_Do_main.cpp` | 6 |
| `jfw_trace` | `dusklight/libs/JSystem/src/JFramework/JFWSystem.cpp` | 20 |
| `aram_trace` | `dusklight/libs/JSystem/src/JKernel/JKRAram.cpp` | 8 |
| `exp_trace` | `dusklight/libs/JSystem/src/JKernel/JKRExpHeap.cpp` | 5 (failure path + `create` entry/exit) |

**Deleted** (recurring or stale):

- the per-frame `loop frame N` counter in `m_Do_main.cpp`
- the per-frame `present()` trace in `vita/aurora/overlay/gfx/vita_gl_replay.cpp`
- all four `tpv_dvd_trace` patches in `vita/aurora/patches.py`

`build/aurora_lib` is byte-identical to the known-good snapshot except `dvd.cpp`, and that diff is
*only* the removed trace lines. Sibling threads are still present as required.

All measurements below were taken on the cleaned build.

---

## 5. Verified facts worth not re-deriving

### 5.1 EFB resolution is 608×448 and that is correct

`dusklight/include/m_Do/m_Do_graphic.h`:

```c
#define FB_WIDTH_BASE (608)
#define FB_HEIGHT_BASE (448)
#if WIDESCREEN_SUPPORT && !TARGET_PC
#define FB_WIDTH  (640)
#define FB_HEIGHT (456)
#else
#define FB_WIDTH  FB_WIDTH_BASE
#define FB_HEIGHT FB_HEIGHT_BASE
#endif
```

The 640×456 widescreen variant is `!TARGET_PC` only, so a `TARGET_PC`/Vita build correctly gets
608×448. `dusk/defaultWindowWidth/Height` are 608/448 too. **This is not a Vita-specific
deviation and does not need fixing.**

### 5.2 Present geometry is correct

In `vita/aurora/overlay/gfx/vita_gl_replay.cpp`, `present()` aspect-fits:

```cpp
const double scale = std::min(static_cast<double>(sPresentWidth) / sEfb.width,
                              static_cast<double>(sPresentHeight) / sEfb.height);
const auto width  = static_cast<GLint>(std::lround(sEfb.width  * scale));
const auto height = static_cast<GLint>(std::lround(sEfb.height * scale));
glViewport((static_cast<GLint>(sPresentWidth)  - width)  / 2,
           (static_cast<GLint>(sPresentHeight) - height) / 2, width, height);
```

With a 960×544 surface and a 608×448 EFB this yields `738×544 at (111, 0)`. Measured content bbox
`x271..1008 y128..671` matches the prediction **exactly**. Pillarboxing is symmetrical (111 px
each side) and correct.

The large black area is *expected*, not a bug: 738×544 within 1280×800 is 39.2% predicted vs
39.26% measured.

### 5.3 `check_vpk.py` passes

```
info: ATTRIBUTE2=0xC (+109 MiB main memory)
info: TITLE_ID=TPLG00001 APP_VER=01.00 TITLE='Twilight Princess' CATEGORY='gd'
7 entries; 0 error(s), 0 warning(s)
```

The script lives with the `vita-port` skill. It is present at both
`~/.roo/skills/vita-port/scripts/check_vpk.py` and `~/.claude/skills/vita-port/scripts/check_vpk.py`.
`README.md` previously hardcoded the `~/.claude` path; that was fixed to `~/.roo` with a note to
substitute your own.

### 5.4 `present_target` and `OSStopwatch` are unused

Neither is referenced by anything compiled from the merged tree. Do not add them to the source
lists to "fix" a link error; the link does not need them.

### 5.5 `JFWSystem::init()` exception wrapper is diagnostic only

`m_Do_machine.cpp` wraps `JFWSystem::init()` in `try`/`catch` that records `what()` and the
`std::system_error` code. It does not swallow: it re-throws. It earns its place because the
original abort was an uncaught `std::system_error` thrown from a context no handler could catch,
and this named the failing call.

---

## 6. Host test suites — all green

Each suite has its own `run.sh`. Run them all from `tp-vita/` with `VITASDK` set.

| Suite | Command | Result |
|---|---|---|
| vertex | `./tests/host/vertex/run.sh` | 71,063 checks, **0 failures**; 4,000 random formats vs reference |
| shader | `./tests/host/shader/run.sh` | 2,000/2,000 configs linked on Mesa, **0 failed**; segment-table, u32-lane, light-slot and matrix-bank violations all 0 |
| texture | `./tests/host/texture/run.sh` | 3,780 checks, **0 failures** (negative control still detected) |
| disc | `./tests/host/disc/run.sh` | 14 + 10,686 checks, **0 failures** |
| printf | `./tests/host/check_printf_formats.sh` | clean — no `%z`/`%j`/`%t`/`%hh` in `vita/` |

Other suites: `tests/host/e2e/run.sh` (87 scenes + teardown; `TPV_E2E_ASAN=1` for AddressSanitizer),
`tests/host/disc/disc_controls.py`, `tests/host/e2e/controls/e2e_ctl.py`.

The shader suite prints warnings about configs exceeding Vita uniform limits — these are
*reported*, not failures; the suite's purpose is to quantify them. Current maxima: `fsVec4=26`,
`vsVec4=153`, `maxAttribs=19`; over Vita caps: 93 configs with fs>16, 104 with vs>128, 4 with
attribs>16.

**Note:** I wasted a long detour hand-building a link line for `shader/test_compile.cpp` (chasing
`xxhash` and three absl TUs) before checking that `tests/host/shader/run.sh` already did it, using
its own `host_stubs.cpp`. The wrapper and stub I wrote were deleted. If you find yourself adding
link flags for that test, **stop and read `run.sh` first**.

---

## 7. Open problems

### 7.1 Heap budget — the retail arena trim was live, and is now gone (2026-11-02)

**Status: the size arithmetic is now sane; the `0xBF18265F` allocation itself is still unexplained.**

What used to happen: one run in seven failed with

```
JKRExpHeap: do_alloc FAILING size=0xBF18265F align=32 heap=archiveHeap
            heapsize=0x8DFF50 used=0x6B4678 free=0x22B794
```

`0xBF18265F` is 3.0 GiB against a heap with 2.27 MiB free — a **corrupted size argument, not
exhaustion**.

**Found this session, and it is a different bug from the one we were hunting.** At
[`m_Do_machine.cpp`](dusklight/src/m_Do/m_Do_machine.cpp:790) `mDoMch_Create()` ran the **retail
GameCube arena trim**, unguarded:

```cpp
uintptr_t arenaHi = (uintptr_t)OSGetArenaHi();
if (arenaHi > 0x81800000 && arenaHi - 0x1800000 > arenaLo)
    OSSetArenaHi((void*)(arenaHi - 0x1800000));
```

`0x81800000` is a physical GameCube address. A Vita process relocates the guest arena, so the
comparison describes a machine that is not there — and **it fired anyway**. Measured in Vita3K:

| | `arena lo` | `arena hi` | `size` | MiB |
|---|---|---|---|---|
| with the trim (old build) | `0x8314c1c0` | `0x859481c0` | 41926416 | **40** |
| trim compiled out (this build) | `0x8314c1a8` | `0x871481a8` | 67092240 | **64** |

The delta is exactly `0x1800000` = 24 MiB. So every run until now handed the game a guest arena
24 MiB smaller than the platform actually provided, and the heaps carved from it (§4.4's bounded
budget) were sized against a figure the hardware never returns. That is a plausible reason the
budget has felt tight and a plausible contributor to allocation failures at room-load time.

The fix is `#if !DEBUG && !TARGET_VITA` around the block, with the measurement recorded in the
comment so nobody re-adds it. Verified: `0x81800000` no longer appears anywhere in
`mDoMch_Create` (`0x810b5648`, size `0xb64`) in the built ELF, and the trace reports 64 MiB.

**What is still open.** This does not explain a 3.0 GiB size argument, and it must not be recorded
as if it did. Still ruled out by arithmetic:

- [`daMP_THPPlayerCalcNeedMemory()`](dusklight/src/d/actor/d_a_movie_player.cpp:3673) — every term
  is `ALIGN_NEXT(..., 32)`, so its result is always a multiple of 32. The observed value has
  `& 0x1F == 0x1F`. **Not the source.**
- `0xBF18265F` as IEEE-754 is ≈ -0.594, which looked promising as a float-as-size bug, but the
  value is not 32-aligned, so it does not come from a 32-aligned calculator.

The failure path in `JKRExpHeap::do_alloc` still records `__builtin_return_address(0)`:

```
exp_trace("do_alloc FAILING size=0x%X align=%d heap=%s heapsize=0x%X used=0x%X free=0x%X "
          "caller=%p", ...);
```

**Next step:** with 24 MiB more arena in place, watch whether the frequency changes at all. If it
still recurs, loop the run and symbolise `caller=%p` with
`arm-vita-eabi-addr2line -e build/tpv_vita`.

### 7.2 BMD loader invalid reads — ROOT-CAUSED AND FIXED (2026-11-02)

**The two recorded addresses were stale and one of the two labels was wrong.** Do not reuse them.
The ELF was rebuilt after §4.4, so both offsets moved, and `+838` did not even land on the
function named:

| Recorded | What the current ELF says |
|---|---|
| `makeHierarchy` +432 = `0x81ba8b48` | `makeHierarchy` starts at `0x81ba87d8` and is only `0x1e6` bytes, so `+432` is **past the end of the function** — it resolves to `J3DModelData::clear()` at `0x81ba8b40`. Not a fault in `makeHierarchy` at all. |
| `loaderBasicBmd` +838 = `0x810e506e` | Mis-added: `0x810e4cb0 + 838` = `0x810e4ff6`. The recorded `0x810e506e` is **inside a literal pool** (`.word 0x8240c400`), i.e. data, not an instruction. |

**The real defect, found by disassembling the function rather than trusting the labels.** In
[`J3DJointTree::makeHierarchy()`](dusklight/libs/JSystem/src/J3DGraphAnimator/J3DJointTree.cpp:24),
`J3DModelLoader` calls `makeHierarchy(NULL, &hierarchy)` — so **`pJoint` is NULL on the top-level
call**. The material and shape arms then read:

```cpp
} else if (newMaterial != NULL && pJoint->getType() == 'NJNT') {
    pJoint->addMesh(newMaterial);
} else if (newShape != NULL && pJoint->getType() == 'NJNT') {
    J3DMaterial* newMaterial = pJoint->getMesh();   // shadows the outer one
    newMaterial->addShape(newShape);                // no null check
```

`J3DJoint::getType()` returns the literal `'NJNT'` and never reads `this`, so `pJoint->getType()`
**succeeds through a NULL pointer** and the compiler is entitled to treat the branch as taken. GCC
did exactly that and folded the next statement into a read of absolute address `0x58`:

```
81ba895a: 2000   movs r0, #0
81ba895c: 6d80   ldr  r0, [r0, #88]   @ 0x58 == J3DJoint::mMesh
```

`0x58` is `J3DJoint::mMesh` (confirmed in
[`J3DJoint.h`](dusklight/libs/JSystem/include/JSystem/J3DGraphAnimator/J3DJoint.h:124)). A
`kTypeMaterial` or `kTypeShape` record appearing at the **top level** of a hierarchy therefore
guaranteed a null dereference. `kTypeJoint` at top level is safe (it sets `mRootNode`).

This is **inherited from upstream, not a port regression**: `J3DJointTree.cpp` is unmodified
relative to dusklight `77705f8`, and the reference decomp in `tp/` has the identical code.

**Fix** (same file, `dusklight` local modification #14):
- Guard both arms with `pJoint != NULL`. A top-level material or shape record is now skipped
  instead of faulting.
- Removed the shadowing `J3DMaterial* newMaterial` in the shape arm (renamed `pMesh`) and added
  an explicit `pMesh != NULL` check, because `getMesh()` can legitimately be NULL.

**Verified after the fix:** the `movs r0, #0` / `ldr r0, [r0, #88]` pair is gone from the
codegen; every `mMesh` access now goes through the live joint register, and the shape arm has an
explicit `cmp.w ip, #0` / `beq` guard before any store. Rebuild is clean; a Vita3K run (`run 30 3`)
reaches `main01: entering main loop`, renders two frames with the exact expected bbox
`(271,128)–(1008,671)`, and shows no allocation failure and no crash marker.

**What remains unproven, and is now weaker than it looked.** Whether these were the reads reported
earlier. The original report lost the faulting instruction, and neither address can be
re-symbolised now.

The DuskOnVita audit (§7.5) made this sharper. That port pins dusklight `34a144a`, three commits
behind our base, and its [`J3DJointTree.cpp`](dusklight/libs/JSystem/src/J3DGraphAnimator/J3DJointTree.cpp:24)
is **byte-identical** to ours — same NULL `pJoint`, same `'NJNT'` constant, same shadowing nested
`newMaterial`. **None of its 34 patches touch J3D, joints, BMD or the hierarchy loader, and it boots
and plays on hardware.** So this defect is real, inherited and worth fixing, but on a well-formed
disc image it is latent rather than routinely taken, and it is **not** established as the cause of
our earlier crash. Correct wording: *a genuine reachable null dereference on this path is fixed;
the earlier log is still unexplained.*

Note that `do_alloc` on `TARGET_PC` **aborts** on failure, so if these reads were the same defect as
§7.1 they would present as `SIGABRT`, not as a graceful failure.

### 7.3 Every texture upload was failing — ROOT-CAUSED AND FIXED (2026-11-02)

This was the largest rendering defect on the board, and it was invisible until the frame was
inspected rather than trusted.

**Symptom.** The game log held **31,786** `GL error uploading` warnings in a single run — which is
effectively *every* texture: `GX Static Texture` (31,062), `GX Static Palette Texture` (710),
`Resolved Texture` (13), `Copy Conv Texture` (1). The frames still "rendered", so this went unnoticed
for two sessions. Decoding the PNGs numerically is what exposed it: red was pinned at **255 in every
row band**, there were **no cool tones at all**, and only 3,093–3,979 distinct colours (dominant
values around `(224,160,96)` and `(224,224,224)`) — the signature of untextured geometry.

**Why the log could not name the cause.** `had_gl_error()` drained the GL error queue and kept only
a bool, so the enum was discarded. It also counted pre-existing errors from unrelated calls.

**Diagnosis (measured, in this order).**
1. Named the code instead of discarding it: it is **`GL_INVALID_ENUM`** for every size, mip count and
   pixel/no-pixel case. That rules out size, NPOT, mip-chain and allocation causes at a stroke.
2. `GL_RGBA` internal format, `GL_RGBA`, `GL_UNSIGNED_BYTE` and a `nullptr` data pointer are all
   legal, and `GL_RGBA` is defined (0x1908) in both `GLES2/gl2.h` and `vitaGL.h`, so the
   `glTexImage2D` arguments are not the problem.
3. Added a one-shot checkpoint after each GL call inside `create_texture`. The error is already
   queued at **`glPixelStorei`**, before `glTexImage2D`. `glTexImage2D` was never failing.
4. Read vitaGL's actual implementation out of the static library:
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
   **vitaGL implements `glPixelStorei` for `GL_UNPACK_ROW_LENGTH` only.** Every other `pname` sets a
   sticky `vgl_error = GL_INVALID_ENUM`. So `glPixelStorei(GL_UNPACK_ALIGNMENT, 4)` was a no-op that
   poisoned the queue, and `create_texture` then discarded the texture it had just uploaded
   correctly.

**Fix** ([`vita_texture.cpp`](tp-vita/vita/aurora/overlay/gfx/vita_texture.cpp:80), overlay — the
build keeps working because the overlay is a whole-file replacement):
- `set_unpack_alignment()` skips the call on `__vita__` and keeps it on the host. All uploads are
  RGBA8, so rows are always 4-byte aligned and the call is unnecessary on Vita; on Mesa it is real.
- `report_gl_errors()` replaces `had_gl_error()`. It names each enum and logs each distinct
  (label, code, size, pixels) **once**, so a broken path yields a short list instead of 30k lines.
- `create_texture` no longer throws a successfully-created texture away.

**Verified after the fix:**
- texture-module warnings: **31,786 → 0**; GL error lines: 0.
- distinct colours: **3,979 → 78,360** and **3,093 → 87,870** (≈20–28×), i.e. textures are now
  visibly applied. Red is no longer saturated at 255 and dark tones appear again.
- bbox still exactly `(271,128)–(1008,671)`; `mDoMch: return 1` and `main01: entering main loop`
  still reached; host texture suite still 3,780 checks / 0 failures with its negative control
  detected.

**Still true afterwards:** the image contains **0.0% pixels with B > R**, i.e. no cool tones at all.
Whether that is correct is unknown — Twilight Princess' opening area legitimately looks warm, but
this cannot be settled without a reference render (§7.4). Do not read "textures upload" as "the
scene is right".

**A latent instance of the same trap remains** in
[`vita_gl_replay.cpp:1334`](tp-vita/vita/aurora/overlay/gfx/vita_gl_replay.cpp:1334)
(`glPixelStorei(GL_PACK_ALIGNMENT, 1)` in `read_efb_rgba`). It is currently harmless because
`read_efb_rgba` has **no callers** — it is dead code. It becomes a bug the moment anything calls it,
and the `1` was the wrong pack alignment for RGBA anyway.

### 7.3c Draw batching: measured, and NOT worth doing — 442 draws only reach 377 (2026-11-05)

Draw-call reduction is the next optimisation this emulator *can* actually resolve, because it cuts GPU-side
work rather than guest CPU work (§7.8 explains why timing numbers here are unreliable and draw counts are
not). It was measured rather than assumed. **The answer is no**, and this section exists mostly to stop a
future session from re-deriving the optimistic number and acting on it.

#### The final measurement

Six samples on real play-scene frames (442 draws, the scene, not a loading frame), identical every time
except for the 422-draw frame:

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
| adjacent `runs` (program+textures) | 211 | state-identical runs in submission order |
| `STRICT runs` (+ uniform offset + `immediateData`) | **442** | every draw is unique |
| adjacent pairs sharing a uniform **offset** | **0** of 441 | nothing is reused by address |
| adjacent pairs sharing uniform **bytes** | 69 of 441 | the only sound test |
| **fully mergeable pairs** (same state **and** bytes) | **65** | |
| **batched call count** | **377** | **longest run 7** |

**442 → 377 calls is 1.17x, not 2.6x.** A 15% reduction in draw calls, bought by rewriting the decode
stage to append into one contiguous buffer with per-source index offsets — a change to the hot path that
Vita3K cannot show a win for (§7.8: 171–504 ms for the same config). The cost/benefit is not close, so
this is **closed**, not deferred.

#### What I got wrong twice, in the same direction

The first survey said `57 pipelineGroups` and I recorded "5.6x". That counted only the pipeline, so it
counted draws with *different textures* as mergeable — they are not. The second survey folded in
`bindGroups.textureBindGroup` and reported `123 adjacent runs` at 320 draws, which reads as **2.6x**, and
that is the number that stood in this document for a while. It was still wrong, and by a wider margin than
the first error: it counted **state-identical** runs, but a `glDraw*` also carries **one uniform set**, and
`gx::DrawData` has per-draw `uniformRange` plus `immediateData`. 211 draws are state-identical and only 65
pairs are actually mergeable.

Both errors are the same mistake: **I stopped at the criterion I had instrumented instead of writing down
the full legality condition first and measuring that.** The lesson is cheap to apply — state the complete
condition in the instrument (pipeline, samplers, uniform content, contiguity) before believing any
aggregate of it.

One sub-finding is worth keeping because it looks like an obstacle and is not: **`currentPnMtx` does not
block a merge.** It is baked into each decoded vertex (`vita_vertex.cpp:427` sets `p.constant`), and the
matrices it indexes live in the shared uniform array, so two draws differing only in PN index can still
merge. Conversely, uniform **offset** is the wrong test — the frame arena reuses storage, so different
offsets routinely hold identical bytes (0 shared offsets, 69 shared blobs). Any future survey must compare
content, not addresses.

#### Two instrument bugs that produced empty or misleading logs

1. The survey was gated on `sFrameIndex == 2760`, but the driving script ends near frame 2400. It never
   fired. Worse, a careless edit had nested the whole 120-frame timing window **inside** that gate, which
   silenced every `window @frame` line too — that is how the empty log was noticed at all. An instrument
   that can silently not run is worse than none: an empty log reads exactly like "nothing to report".
2. The next gate was `frame.draws.size() >= 100`, which fired at frame 240 — a **loading** frame with 163
   draws, not the 442-draw scene. A threshold low enough to always fire is low enough to fire on the wrong
   frame.

Final form: sample up to 6 frames that have `>= 350` draws, print one line per sample, and latch on a
count rather than on a frame number, because a frame number is a hidden dependency on how far the script
happens to get. `kSurveyMinDraws = 350`, `kSurveyMaxSamples = 6`.

#### Status of the code

The survey is **left enabled** in `vita_gl_replay.cpp` for now: it costs six log lines per run and it is
the evidence for this section. It should be deleted, not left to rot, once this record is trusted — the
GX replay path is the hot path and instrumentation there is not free.

### 7.3b Per-draw allocation in `bind_fog_range`, removed (2026-11-05)

A per-draw waste, found by reading the replay path rather than by profiling.

`bind_fog_range()` runs for **every draw that has fog-range adjustment enabled** (~440 draws/frame in
the play scene) and constructed a fresh `std::vector<uint8_t> rgba(width * 4)` on each call to build the
LUT — one heap allocation plus a fill per draw, thrown away immediately. It now rebuilds into a reused
`sFogRangeScratch` member, the same pattern `sScratch` already used for `decoded` / `chunks` /
`uniforms`. `resize()` does not shrink capacity, so after the first few draws the steady state allocates
nothing. The `sFogRangeRgba` comparison that skips an identical upload is unchanged, and that member
still stores contents rather than a reference.

**There was no state-cache win available.** `apply_pass_state`, `use_program`, `bind_texture` and
`enable_attribs` were all already gated on `sCache`. Worth recording because the one call that *looks*
wrong — `set_enabled(GL_DEPTH_TEST, sCache.depthTest, true)` — is correct: the signature is
`set_enabled(cap, int8_t& shadow, bool enable)`, so the cache entry is the by-reference shadow and `true`
is the desired value. I read it backwards first and nearly "fixed" working code. Check the signature.

**Verified:** host `e2e` **87 scenes, 14,242 checks, 0 failed**; host `texture` **3,780 checks, 0
failures** with its negative control still detected; Vita3K run yields **0** `fog range LUT out of range`
warnings, **0** invalid accesses, and frame content structurally identical to the previous run — same
39.21 % deltas across boot/attract/title, same ~0.2 % on the static card screens, then 36.2–36.3 %
advancing through the name screens into a moving scene.

**The timing result is NOT a proven win, and is recorded as unproven deliberately.**

| window | before | after |
|---|---:|---:|
| frames 1200–1800 | 212.7 ms | 200.1 ms |
| frames 2400–2760 | 218.6 ms | 162.0 ms |

That looks like a large gain, but §7.9 already recorded **171–504 ms for the same configuration**, so one
before/after pair cannot separate this from the emulator's own noise. The mechanism is real and free (one
fewer allocation per draw) and it is the kind of change that matters on a Cortex-A9, where the allocator
is not a fast host libc — but **do not quote a percentage from this**. Settling it needs repeated runs or,
better, hardware.

### 7.4 Not started

- Real hardware testing (nothing has run outside Vita3K).
- Audio (currently `DUSK_AUDIO_DISABLED`; the audio engine is told it initialised, which is a lie
  to get past boot).
- ~~**A reference render.**~~ **De-scoped 2026-11-09**: the user validates the picture from the run's
  screenshots by eye, which is what §3d.6's gameplay frames are for. The old text called this "the
  highest-value item" because every visual judgement here would otherwise be a guess; that is now
  answered by eye, not by a desktop build.
- Card saves, pause menu, gameplay content.

### 7.4b Upstream status and the three fixes taken from DuskOnVita (2026-11-10, extended)

**Upstreams are quiet. Verify before re-deriving:**

| Repo | Our pin | Upstream tip | New commits since our pin |
|---|---|---|---|
| `TwilitRealm/dusklight` | `77705f8` | `77705f8` | **0** |
| `encounter/aurora` | `3227d76` | `f77492f` | **24** |

So there is nothing to pull for dusklight, and the 24 aurora commits cannot be taken: they are the
`aurora::input` migration plus `505b5eb`'s deletion of 22 public `PAD*` functions, which breaks
dusklight's own sources (see the header comment in `vita/aurora/patches.py`). **DuskOnVita is on the
exact same dusklight pin** (`77705f8`), so its aurora work is directly comparable rather than
ahead of us.

**Three correctness fixes taken from DuskOnVita / upstream aurora**, each verified against the source
and against our own build's numbers rather than trusted:

1. **GX min-filter table had two entries swapped.** `kHwToGxFilter` in `overlay/gfx/texture.hpp` is the
   inverse of `GX2HWFiltConv = {0x00,0x04,0x01,0x05,0x02,0x06}`, so hardware bits `0x2` must decode to
   `GX_NEAR_MIP_LIN` (enum 4) and `0x5` to `GX_LIN_MIP_NEAR` (enum 3). Both entries were swapped, so a
   texture set to linear minification with the nearest mip came back as point minification blending two
   mips, and `gl_min_filter` sent it to `GL_NEAREST_MIPMAP_LINEAR` instead of
   `GL_LINEAR_MIPMAP_NEAREST`. **Active in our build** — this is nearest sampling where the game asked
   for linear, i.e. shimmering textures in motion. Fixed; same defect as upstream `26371ce` (#293).
2. **Light directions were not normalised.** Our generated fragment shader uses the raw direction in the
   diffuse terms (`vita_shader.cpp:780` `dot(mv_nrm, ldirL)`, `:771` `dot(ldir, ldirL)`) while the emboss
   path in the same file *does* call `normalize`. Dusklight stores whatever the game passes to
   `GXInitLightDir`, which is not guaranteed unit length, so light intensity was scaled by the vector's
   magnitude. **Active in our build.** Fixed by normalising each light in `gx/shader_info.cpp` before the
   bulk append (upstream's `0b39c95` cannot be taken verbatim: at our pin the whole light array is
   appended in one call, so there is no per-light site to wrap).

3. **The save path allocates 64 KiB on an 8 KiB stack.** `copy_stream()` in aurora's `io.cpp` has a
   `std::array<uint8_t, 64*1024>` local, and every card write goes through it (`open_atomic_file` then
   `copy_stream`, called from `CardRawFile.cpp` and `FileIO.cpp`). The thread it runs on is the memory-card
   thread from `m_Do_MemCard.cpp`, whose `STACK_SIZE` is **`0x2000` (8 KiB)** -- `0x1000` only under
   `PLATFORM_GCN` -- and dusklight's `OSCreateThread` runs the thread body on a detached `std::thread`, so
   that is the real stack the body runs on. The static `MemCardStack` array is bookkeeping, not the thread's
   stack. 64 KiB against 8 KiB is an 8x overflow on the first write. This is the defect DuskOnVita hit on
   hardware as a data abort (their patch `0005-vita-copy-stream-stack`), and the numbers are ours, not
   theirs. **Active in our build**, and it sits on the save path that section 2 still lists as untested: on
   hardware it would have presented as "saving crashes", with no address clue pointing at a 64 KiB local.
   Fixed to 4 KiB chunks (the loop already handles short reads, so only throughput changes). **Vita3K cannot
   reproduce this** -- host thread stacks are megabytes -- so no emulator run would ever have shown it.

**One latent fix worth nothing yet:** negative LOD bias was cast float->`u8`, which ARM saturates to 0
(upstream also fixed this). Patched in `patches.py`, but **`kUseTexLodBias = false` in
`vita_shader.cpp`, so nothing consumes it and no pixels change.** Recorded as latent so nobody
credits it with a rendering improvement later.

**Verified:** clean build, VPK produced, `e2e` **91 scenes / 14,266 checks / 0 failed**, `texture`
3,780 / 0 failures with its negative control still detecting an expected mismatch. **Not verified:** no
pixels were compared on a real scene, so none of the three fixes has been shown to improve the picture --
only that none can regress the host suites.

**The decoding fixes now have tests, and getting that right took a correction.**
`tests/host/e2e/scenes_texobj.hpp` adds four scenes that drive the real `GXInitTexObjLOD` / `GXInitTexObjFilter` / `GXInitTexObjLODBias` /
`GXLoadTexObj` through the real FIFO and command processor, then read back what `gxState().loadedTextures`
decoded. Before this, `grep -rn 'min_filter|lod_bias' tests/host/` was empty, so both defects could have been
reverted the same day with every suite still green. Fix 3 is not tested here -- it is a stack-depth
constraint, which no host run can observe (host stacks are megabytes); it rests on the arithmetic above.
Proven by reverting each defect in turn:

- the swapped min-filter table is caught by the round-trip scene, in exactly the two slots it corrupts
  (`GX_TEXMAP3 min_filter = 4, expected 3` and `GX_TEXMAP4 min_filter = 3, expected 4`);
- **the LOD-bias UB is not detectable at runtime on x86 at all, and my first attempt at it silently passed
  with the defect present.** That is worth writing down, because the reason is subtle: at runtime gcc emits
  `cvttss2si` then stores the low byte, so `(uint8_t)(32.0f * -4.0f)` gives `0x80` -- the correct answer --
  in *both* the buggy and the fixed form. It is only at compile time that gcc folds the same expression to
  `0`. Same expression, two different answers: the signature of UB, and the same divergence that ARM's
  saturating conversion produces on hardware. The fix is therefore pinned by `static_assert`s over a
  `constexpr` encoder instead, where the buggy form is ill-formed (`overflow in constant expression`) and
  the fixed form is well defined because integral conversions are modulo. Verified both ways: the build
  fails with those two assertions when the defect is reintroduced.

**What was audited and deliberately NOT taken** (so a future session does not redo it). DuskOnVita carries
17 aurora patches, 53 dusklight patches, 2 borealis and 5 vitaGL. Almost all are performance work written
against *their* GXM backend and do not map onto our vitaGL one -- `command_processor.cpp` register-decode
fast paths, static display lists, a GX worker thread, frame overlap. Specifically:

- **vitaGL patches (3 substantive):** not takeable as patches -- we consume vitaGL as a prebuilt
  `libvitaGL.a` from VitaSDK, not as source. Their uniform-reservation and sampler-object changes are
  performance/safety work on vitaGL itself; adopting them would mean building vitaGL from source, which is
  a project, not a cherry-pick.
- **`borealis/0002` (device paths are absolute):** real defect class, but only affects `borealis::data`,
  which our build does not use for the game's paths (we hand aurora absolute `ux0:` paths directly).
- **abseil patch:** byte-for-byte the same two hunks as our own `patches/abseil-vita.patch`. Nothing to take.
- **`aurora/0012` `lib/gfx/texture.hpp`:** identical fix to the one already applied here; we additionally
  took the idea for the tests.
- **`aurora/0005`** is fix 3 above, the one substantive port.
- **Upstream aurora's 24 commits:** only four were candidates and all are no-ops for Twilight Princess.
  `f77492f` (truncate vertex count, skip 0 draws) is ordering, not correctness, for us -- the decode already
  clamps to `availBytes / vtxStride`, `draw_op` already returns early on `vtxCount == 0`, and GL ignores
  incomplete primitives. `6dc60dc` (`GXSetZScaleOffset`), `8aa4c09` (`GDSetTexImgPtr`) and `9873709`
  (`GXClear`/`GXClearSubrect`) add functions the game never calls: `grep -rn GXSetZScaleOffset dusklight/src/`
  finds no caller, and TP has its own GX lib underneath. The rest are `aurora::input`, WPAD/KPAD, SDL,
  `OSUtf`, `AURORA_RVL` and the presentation API -- all behind the `505b5eb` migration wall.

**A THIRD port exists, it RUNS IN VITA3K, and it rendered the reference frame this project
de-scoped (2026-11-11).** `capitanbarbacoa` publishes "Dusklight PSVita" at
<https://capitanbarbacoa.itch.io/duskgxm> (v1.1.0, `Dusklight-1-1-0.vpk`, 22 MB, built 2026-11-03,
AI-assisted, code not shared). The workspace holds a copy at `../Dusklight-1-1-0.vpk`.

**It was run, not just read.** `TPV_VPK=... tests/vita3k/run.sh run 40 4` boots it in the same harness we
use for ours, on the same `GZ2P01.iso` (symlinked to `ux0:data/dusklight/Zelda.iso`, the path it expects).
Result, saved as `tests/vita3k/screenshots/theirs_prelaunch_{1..4}.png`:

- it reaches its own prelaunch/preset screen **on top of a fully rendered 3D scene** - the Ordon
  spring backdrop, textured and lit, with the HUD-free scene visible around the modal;
- consecutive frames differ **30% of the scene-crop pixels (max-abs-diff 149)**, so it is a live scene,
  not a frozen frame; screenshot sizes (390-400 KB) sit in the same range as our gameplay frames;
- it does **not** crash in the emulator (it did on a fresh firmware-less work dir, which was a setup
  fault: it wants `ur0:data/external/libshacccg.suprx`, and our own runs only warn on that same
  fallback, so the emulator's own `ur0:data/libshacccg.suprx` is enough).

**Two of my earlier claims in this section were WRONG and are retracted here:**

1. *"The `eboot.bin` is encrypted (`flags 0x100c0`), so it is not analyzable."* **False.** Our own eboot,
   which we build ourselves, has the **identical** `flags 0x100c0` - that is not an encryption marker.
   The real reason `strings` finds nothing in either file is **compression**: valid zlib streams start at
   `0x1000` (verified: 420 KB, 184 KB and 582 KB decompress cleanly). A plaintext `\x7fELF` sits at
   offset `0xa0` in both. So it is a compressed-but-readable SELF, and it **could** be unpacked; this
   session did not do that, and it should be treated as a separate, deliberate task rather than a
   "closed door". The lesson is the one this project keeps relearning: **validate an instrument against a
   known-good input first** - comparing against our own eboot took one command and would have prevented
   both wrong claims.
2. *"`res/rml/vita.rcss` does not apply to us because we stub RmlUi entirely."* True today, but it
   understates what their run proves: **the Dusk UI works on this GPU in the emulator**, prelaunch and
   all. It is not a limitation of the hardware or of aurora; it is a gap in *our* port, and their
   `vita.rcss` is the measured, ready-made answer for the part that would be slow.

**Their eboot WAS unpacked, and it says one thing that matters (2026-11-11).**
`tools/unpack_self.py` decompresses a SELF in place. It is validated against our own build first,
which is the whole point: the output must contain `Aurora (vitaGL) initializing`,
`ux0:data/tpvita/shader_fail` and `input script: ACTIVE`, three strings visible in our source. It does, so
the unpacker is trustworthy before it is pointed at a foreign file.

Two traps are documented in that script because both cost time here. `SceHeader` flags `0x100c0` do
**not** mean encrypted (our own eboot carries the identical value). And the program headers are **not**
`Elf32_Phdr` even though `e_phentsize` is 32 -- `p_offset`/`p_vaddr` are SCE segment descriptors, so
walking them yields plausible nonsense (`type=0x0159a284`). What works is scanning for zlib stream headers
and decompressing each stream in place; the streams are contiguous. Result: 31 MB of readable image for
ours, 32 MB for theirs.

**What the analysis found, and what it did not.** Their build is **stripped**: `aurora::gx::` appears 5
times in ours and 0 in theirs, `dusk::` 26 vs 1, and the GXM/vitaGL entry points are imported by NID rather
than named. So there is **no symbol-level comparison to be had**, and this should not be repeated expecting
one. What *is* readable is a string-level structural difference, and it is significant:

| | ours | theirs |
|---|---|---|
| WGSL source as literals (`@fragment`/`@vertex`) | **0** | **5** |
| `textureSample` (WGSL builtin) | **0** | **15** |

**They ship upstream aurora's WebGPU shaders as WGSL text and translate them to GXM at runtime; we
generate GLSL from the GX state instead.** That is the architectural difference between the two ports, and
it explains a lot at once: it is why their patch set is 17 careful edits to aurora's `lib/gx/` (they are
keeping upstream's shader path intact underneath), why their `GXTexture`/filter fixes were the same ones
upstream was making (they track upstream), and why our two active fixes were needed at all -- on our route
the GX-to-GLSL generator is *our* code, so upstream's shader fixes do not reach us and defects like the
swapped min-filter table are ours to find. It is also the honest answer to "should we switch routes?":
theirs is closer to upstream and gets upstream's fixes for free, ours avoids a WGSL-to-GXM translation
layer on a Cortex-A9. Neither is obviously right, and this is not evidence to switch -- only evidence of
what the difference costs and buys.

**Their screenshots are NOT yet a valid reference for our rendering, and this is the trap to avoid.**
The frames exist and are live (verified), but they show a **different scene at a different time of day**:
theirs is the prelaunch backdrop, ours are in-game dialogue and the outdoor field. Comparing region
luminance between them would be meaningless, and this project has already published wrong conclusions twice
by comparing things that were not comparable (HANDOVER section 7.8). To use their build as a reference
properly, the next session must drive **both** ports to the *same* point -- same scene, same camera, same
frame -- which their preset/Play UI makes possible but was not done here. Until then, treat their frames as
proof that a correct picture is reachable on this GPU, not as a pixel reference.

What it establishes, and what it does not:

- **It is a hardware-tuned reference for the same GPU.** Their notes: aiming for at least 25 FPS, about
  80% of the game at 30 FPS, requires an **overclocked** console with settings high, and they recommend
  reducing resolution. That is the honest performance ceiling for this game on a Vita, from a third
  independent implementation, so **do not expect 30 FPS unclocked at native resolution** from this port.
- **The `eboot.bin` is NOT analyzable and was not attempted.** The SELF header is encrypted
  (`flags 0x100c0`), so a naive segment-table parse yields garbage (it "found" 5922 segments — that was
  ciphertext being read as a struct). VitaSDK ships `vita-make-fself` only, no inverse. Reading it would
  need SCE keys, which is out of scope; the `param.sfo` and the `res/` tree are readable and that is all
  that was used.
- **`res/rml/vita.rcss` is a hardware-measured list of what to remove from the Dusk UI**: every
  `backdrop-filter`, `box-shadow` and `filter:` becomes a flat background or a border, with the numbers
  attached (a 22-mod catalog page took **8.5 s** to build; GPU frames of **300-600 ms** from
  `GeometryBoxShadow`/`SaveLayerAsTexture` alone, each shadow rendered into its own layer and texture,
  game thread waiting on the GPU). **Today our build renders no RmlUi documents at all** - it compiles
  exactly one file from `src/dusk/ui/` (`icon_provider.cpp`) and stubs the rest
  (`vita/src/dusk_ui_icon_provider_stub.cpp`, CMakeLists.txt:203) - so there is nothing yet for this file
  to fix. **But their run shows the UI working on this GPU in Vita3K**, so the gap is ours, not the
  hardware's or aurora's, and this is the file to copy the day the Dusk UI is wired up here.
- **They ship a precompiled shader cache and we do not.** `shader_paketote.bin` (5.75 MB, magic `DSPK`,
  ~4334 entries at 16 bytes each, obfuscated payload) is a *cold* cache baked at build time. Ours is only
  a *warm* one: `vglSetShaderCachePath("ux0:data/tpvita/shader_cache")` persists what the console has
  already translated, so the **first** run pays full price. §7.8 measured that price at **200-700 ms per
  program** with the first ~70 built lazily, i.e. tens of seconds of stutter spread over the opening
  scenes, on a device where the player just launched the game. Vita3K cannot show this (its translator is
  a host compiler), and **nobody has yet verified that our cache file is even written on a real Vita** —
  `grep -rn shader_cache tests/` finds no check. When hardware arrives, this is the first thing to measure,
  and it is worth more than any of the micro-optimisations in §7.8/§7.9.

**What DuskOnVita has that we should ask about, not re-derive.** Their console results are the thing our
Vita3K-only workflow cannot produce: GX worker time down 6.6 ms in Ordon village, 8.0 in Kakariko,
13.6 in Hyrule Field (18 → 28 FPS); `C_MTXConcat` and J3D envelope sums through NEON, verified bit-exact
over **10.6 million** products, 1.5–2.3 ms off Castle Town's main thread. Every one of their fast paths
ships with a `--vita-fast-*=verify` mode that compares against the original code path, which is the
pattern §7.8 says we need and Vita3K cannot supply. Their `NathanKneT` offered a two-way exchange on
issue #1: our synthetic GX scenes as a regression suite for their GXM backend, their hardware bench and
start-up/save-safety code for us.

### 7.5 The DuskOnVita audit: what another port already answered (2026-11-02)

[`NathanKneT/DuskOnVita`](https://github.com/NathanKneT/DuskOnVita) is an independent,
MIT-licensed Twilight Princess Vita port — **not a fork of ours**, and materially further along. It
is the reference implementation for everything left on our board. Clone it to `/tmp/DuskOnVita` to
read it; nothing from it is vendored here.

**What it establishes, with console measurements (PS Vita 1000, 28 Oct 2026, build b2):**

| Scene | FPS | Bottleneck |
|---|---:|---|
| Ordon, outside Link's house | 27.9 / 27.4 | GX worker |
| Castle Town, twilight | 21.7 / 19.4 | main thread |
| Faron fight | 18.3 / 14.6 | main thread |
| Hyrule Field bridge | 17.6 / 19.2 | main thread and GX worker |
| Kakariko, twilight | 12.1 / 12.7 | main thread |
| Ordon village centre | 11.9 / 12.5 | main thread |

Its first build (22 Oct) did **6.3 FPS**; this is 4x in six days. It reaches the title screen, starts
a new game, plays the Ordon opening, saves, and runs at 444 MHz via `scePower` with no plugin,
rendering at 75 % (720x408) scaled to 960x544, with game ticks pinned to 30 Hz so speed holds down
to 15 FPS.

**Conclusion for us: the port is feasible on real hardware.** That question is now closed, and it
never needed to be answered by our own VPK.

Where the two ports differ, and what each choice bought:

| | `tp-vita` | DuskOnVita |
|---|---|---|
| Integration | merged tree built at configure time (`tools/build_aurora_tree.py`); upstream checkout untouched | git submodule + `patches/` applied to a build *mirror*; submodules never patched |
| Local delta | 14 dusklight edits, 1 aurora overlay | **34** dusklight, 11 aurora, 2 borealis, 5 vitaGL patches |
| Renderer | vitaGL only | vitaGL **and** a hand-written SceGxm backend (`gfx_gxm/`), GXM the default |
| Deps | `tools/fetch_deps.sh`, no hashes | `deps.lock` with **SHA-256 per archive**, digest-pinned VitaSDK container, network off |
| vitaGL | SDK prebuilt `libvitaGL.a` | built from pinned source `7635e3c` |
| Tests | host suites, no goldens | ~100 host tests plus `tests/fixtures/baseline/<sha>/` frozen goldens |
| Base | dusklight `77705f8` | dusklight `34a144a` (3 commits older) |

**Independent confirmation of §7.3.** Its
[`gpu_api.cpp:431`](https://github.com/NathanKneT/DuskOnVita/blob/main/platform/vita/gfx_vgl/gpu_api.cpp)
carries the comment *"vitaGL's unpack API supports row length, not GL_UNPACK_ALIGNMENT. Every upload
here is tightly packed RGBA8, already four-byte aligned."* It never calls `glPixelStorei` at all.
Two ports found the same trap by different routes — that is what makes it worth promoting to the
skill rather than treating as a local quirk. It also drains `glGetError()` once per frame and keeps
the enum, which is the rule in §8 trap 15.

**Its open bug is our open bug.** "Some textures flicker", and its request for help is *"a matched
Dolphin capture of the same scene"* — the same missing artefact as our §7.4 reference render. It
also notes it *"skips draws that would read an empty copy rather than drawing them black"*, which
tells us the saturation we saw was an **EFB-copy read path**, not the texture path; both ports hit
that wall, and it chose to skip rather than sample.

**Taken from it (all done this session):**

- The `#if !TARGET_VITA` guard on the retail arena trim (§7.1). Their `dusklight/0009` states the
  reason plainly: *"the retail physical-address check that removes 24 MiB must not run on relocated
  Vita pointers"*. **Our version was live and removing 24 MiB on every run**; theirs never was,
  because they guarded it from the start.
- `NOTICE` plus `LICENSES/`, recording every pin and licence, and stating the vitaGL LGPL position
  honestly instead of leaving it implicit.
- The golden-fixture idea for tests, and `--vita-exit-after` / `sequence.txt` for self-terminating
  repeatable bench runs (we currently depend on an external timer).

**Deliberately not taken:** its 34 dusklight patches. Those are whole-game optimisation work that
assumes behaviour may be rewritten per system, which contradicts the "behaviour comes from the
decompilation" posture this port has held throughout. We cannot validate them, and pulling them in
would make every future upstream diff unreadable. Read its `docs/` as a specification for what to
optimise, not as code to import.

---

## 8. Traps and gotchas

1. **`tp-vita/` is a git repo now** (`Yannis100/tp-vita`, public), but `build/` and `deps/` are
   gitignored, so **a commit does not protect the tree you actually build from**. Before a sweeping
   change still snapshot the build products:
   `tar -czf ../snapshot-$(date +%Y%m%d)/aurora_lib-merged.tar.gz -C build aurora_lib`.
   There is a precedent at `.snapshot-pre-aurora-upgrade-20261029/` with
   `aurora_lib-merged.tar.gz`, `dusklight-local-changes.patch`, `dusklight-status.txt` and a
   known-good `tpv_vita.known-good.vpk`.
   **Also:** the dusklight fixes (including §7.1 and §7.2) live in the separate `dusklight/`
   checkout, which is **not** part of the `tp-vita` remote. `git log` in `tp-vita` will not show
   them, and `git status` there is clean while the game is still patched.
2. **Never edit `build/aurora_lib`.** It is regenerated. Edit `overlay/` or `patches.py`.
3. **`patches.py` anchors must match exactly once.** A silent no-match is a build failure by design.
4. **A short Vita3K run gives blank screenshots.** Use `75 4`, not `40 2`, before concluding anything.
5. **Delete `dvd_trace.txt` before each run** or you cannot distinguish old lines from new.
6. **On `TARGET_PC`, allocation failure = `abort()`.** `CRASH` → `OSPanic` → `Log.fatal`
   (`[[noreturn]]`). There is no soft failure path. Do not write "handle the null" code expecting
   it to run.
7. **Symbolise the fault PC before theorising.** `r2 == 6` at `_kill_r` means `abort()`, not a
   segfault. That single check distinguishes "my logic is wrong" from "an assertion fired".
8. **The IDE constantly reports bogus C/C++ errors** in these files — `initArena2` undefined,
   `JKR_NEW_ARGS` undefined, `DUSK_GAME_DATA` not a type, `SDL3/*` missing, `fmt/base.h` missing,
   `class "JKRExpHeap" has no member getName`, `GXRenderModeObj is not a type name`. These are
   **includePath artefacts of clangd, not build errors.** The real build (arm-vita-eabi GCC) is
   clean. Do not chase them; confirm with an actual build.
9. **`<aurora/gfx.hpp>` is unusable on the host** — it includes `<webgpu/webgpu_cpp.h>`. This is the
   same reason `patches.py` swaps out the `gx.hpp` WebGPU include for the port.
10. **Do not add `-I` paths or link objects to the host tests by hand.** Each has a `run.sh`.
11. **`dusklight/` modifications are uncommitted.** 14 files are modified relative to
    `77705f8`. If you ever need to re-clone dusklight, those changes are lost unless you capture
    them (`git -C dusklight diff > /tmp/dusklight-local.patch` first). The 14th, added 2026-11-02,
    is `libs/JSystem/src/J3DGraphAnimator/J3DJointTree.cpp` (the §7.2 null-dereference fix).
12. **Do not trust a recorded guest address without re-symbolising it.** §7.2 carried two
    addresses that had gone stale across a rebuild, and one of them (`+838`) had been mis-added on
    top of that. Check that the address lands inside the named function's `nm` size, and check that
    it decodes as an instruction rather than literal-pool data.
13. **`J3DJoint::getType()` returns the constant `'NJNT'` and never reads `this`.** A null check
    written as `pJoint->getType() == 'NJNT'` is not a null check: the compiler may treat the branch
    as taken and fold the following member access into a read of a small absolute address. Test
    `pJoint != NULL` explicitly.
14. **vitaGL's `glPixelStorei` accepts `GL_UNPACK_ROW_LENGTH` only.** Any other `pname`, including
    `GL_UNPACK_ALIGNMENT` and `GL_PACK_ALIGNMENT`, sets a sticky `vgl_error = GL_INVALID_ENUM`
    without doing anything. Because these sets are sticky, a *later* `glGetError()` after an upload
    or draw reports a failure that belongs to an earlier, harmless-looking call. Never call
    `glPixelStorei` for alignment on Vita — RGBA8 rows are already 4-byte aligned. Verify the
    library's behaviour from the archive, never from the header:
    `arm-vita-eabi-ar x $VITASDK/arm-vita-eabi/lib/libvitaGL.a textures.o && arm-vita-eabi-objdump -d textures.o | sed -n '/<glPixelStorei>:/,/^$/p'`.
15. **A GL error boolean is worthless; keep the enum.** `had_gl_error()`-style helpers hide the code,
    and a code such as `GL_INVALID_ENUM` immediately rules out an entire class of causes. Log each
    distinct `(label, code, size)` once instead of every occurrence, or a single broken path will
    bury 30k identical warnings in the game log.
16. **GameCube *physical* addresses still compare against relocated Vita pointers, and the compares
    can pass.** `OSGetArenaHi() > 0x81800000` looked obviously dead on a Vita; it was live and it
    removed 24 MiB of guest arena per run (§7.1). The same class of bug hides in any unguarded
    check against a fixed GameCube address — arena limits, MEM1/MEM2 ranges, `0x80000000`-based
    pointer validation, `OSPhysicalToCached`. **Do not reason about these branches, measure them.**
    The cheap test is to log both operands either side of the branch and compare against the same
    run with the block compiled out; here the tell was `0x1800000` appearing exactly in the delta.
17. **A defect being real is not evidence that it caused the symptom.** §7.2's null dereference was
    genuine, reachable and worth fixing, but another port ships the identical source and boots, so
    it cannot be claimed as our crash cause. Before writing "root-caused", ask whether a second
    independent build that lacks the fix also lacks the symptom. If one exists, downgrade the claim
    to "latent defect fixed, causality unproven".

---

## 9. Files the port owns (`tp-vita/`)

Structure, so you know where to look:

```
tp-vita/
├── CMakeLists.txt              top-level build; text-gap check; VPK packaging
├── PLAN.md                     route to a playable port; milestones M1..M5
├── README.md                   build, install, run instructions
├── HANDOVER.md                 this file
├── NOTICE                      every component, its exact pin and its licence
├── LICENSES/                   the upstream licence texts NOTICE refers to
├── cmake/
│   ├── aurora.cmake            aurora core sources
│   └── aurora_gx.cmake         GX frontend sources
├── vita/
│   ├── src/                    main.cpp (entry), boot_diag.hpp (why-boot-failed screens), text_pad.cpp
│   └── aurora/
│       ├── backend.cpp         Aurora backend wiring
│       ├── aurora_stubs.cpp    stubs for unused aurora entry points
│       ├── patches.py          exact-once patches onto upstream aurora/lib
│       └── overlay/            whole-file replacements
│           ├── gx/             texture.cpp, vita_shader.cpp, vita_gx.hpp, vita_vertex.cpp, ...
│           └── gfx/            vita_gl_replay.cpp, vita_frame.{hpp,cpp}, vita_texture_convert.hpp, ...
├── tools/
│   ├── build_aurora_tree.py    assembles build/aurora_lib; enforces patches match exactly once
│   └── check_text_gap.py       link-time text-gap check
└── tests/
    ├── host/{vertex,shader,texture,disc,e2e}/   host test suites, each with run.sh
    └── vita3k/                 headless emulator runner (run.sh, README.md)
```

The interesting Vita-specific rendering work lives in
`vita/aurora/overlay/gfx/vita_gl_replay.cpp` (the GLES2 replay of Aurora's recorded frame) and
`vita/aurora/overlay/gx/vita_shader.cpp` (GX TEV/lighting → GLSL ES 1.00 translation).

---

## 10. Suggested reading order for a new session

1. This file.
2. `tp-vita/PLAN.md` — milestones and the honesty section.
3. `tp-vita/README.md` — build/run, and what a pass proves.
4. `tp-vita/tests/vita3k/README.md` — what an emulator run proves and does not prove.
5. `tp-vita/vita/aurora/patches.py` — read the header comment; it explains why `505b5eb` must not
   be adopted until dusklight migrates.
6. `tp-vita/vita/aurora/overlay/gfx/vita_gl_replay.cpp` — the renderer.

---

## 11. If you have to choose one thing to do next

**Get the picture validated by eye, and stop optimising blind. (2026-11-05 decision.)** §7.4 used to say
"build the desktop reference render in WebGPU/Dawn and diff it". That is now **de-scoped**: the user
validates the picture themselves from the run's screenshots. The consequence for a new session is that
the deliverable changed shape — it is no longer "produce a reference image", it is **produce runs whose
screenshots can be judged**, which means:

- drive far enough that the screenshots show gameplay, not the title or the name grid;
- save them somewhere durable, with the frame the game was on, because §3d.6's whole lesson is that a
  picture without its provenance gets described wrongly later;
- do not describe a screenshot as "the scene looks right" unless it was compared to something. The
  §3d.2 correction exists because I did exactly that.

**Do not spend more time on frame-rate micro-optimisations in Vita3K, and draw batching is now closed.**
§7.8 and §7.9 both ended with the same answer: this emulator cannot resolve a change worth 1-2 ms
because its own noise is ±100 ms, and it runs guest ARM on a host roughly an order of magnitude faster
than a Cortex-A9, so CPU gains measured here do not transfer. The vertex cache in §7.9 is the worked
example: correct, 74% hit rate, halves the decode work, and still not a measurable win on the emulator.

Draw batching was the last candidate that could have shown up even here, because it cuts GPU-side work
rather than guest CPU work. It was measured properly and it is **not worth doing**: 442 draws reach 377
calls, 1.17x, for a rewrite of the decode hot path (§7.3c). It is closed on evidence, not deferred. If a
future session wants a rendering win on the emulator, the honest position is that there is not one left
that this tool can prove — the remaining levers (EFB copies, vertex decode, static geometry by
reference, the GX worker thread) are all §7.8-shaped.

Second, if a run dies: §7.1's size arithmetic is now sane (the arena is 24 MiB larger, §7.1), so
watch whether the `0xBF18265F` failure rate changes at all before doing anything else with it. The
instrumentation already records `caller=%p`.

For reference, the honest position as of this revision: the port boots, reaches the main loop and
renders correctly-shaped frames with plausible colour counts on an emulator. **A separate port does
the same thing on real hardware at 12-28 FPS with working saves and input (§7.5), so feasibility is
no longer in question.** What remains unproven here is scene correctness (no reference image),
performance, audio, and hardware behaviour.

**Corrections to the two claims that used to end this section (2026-11-03).**

- The "roughly 1 run in 7" allocation failure was measured on an older tree. 14 consecutive release
  runs afterwards produced no fatal at all (§3c). Either it was fixed by an intervening change
  (§7.1's arena fix is the plausible candidate) or its rate is low enough that 14 runs prove nothing.
  Both fit the data; neither is proven.
- More importantly, those runs were **idle**, and an idle run never leaves the title/attract loop, so
  none of them played the game. The port currently **stops dead at a GameCube memory-card prompt** and
  cannot be played past file select at all (§3d). That is now the top blocker, and it was invisible
  for the whole project because no test had ever driven the game with input.

### 7.6 First performance baseline, and why EFB copies are *not* the missing piece (2026-11-02)

**A `glReadPixels` probe of EFB copies was attempted and is VOID. Do not repeat it.** The idea was
sound — read the EFB before a copy and the destination after, and compare — but in this context
`glReadPixels` returned **all zeros even for the default framebuffer (fbo 0) at frame 600**, at a
moment when the captured PNG proves the presented surface has content. A control read at fbo 0 was
added specifically to catch this, and it is what saved the measurement from being misreported: without
it, "SRC all zero, DST all zero" would have looked like a broken EFB copy, and it is not.

**What the counters did establish, and this is solid:** the resolve path is very much alive.

| Measure | Value |
|---|---|
| Resolve (EFB copy) ops, first 1000 frames | **11,338** |
| Per frame | **~11** |
| Copies that reached `draw_quad` | 11,338 / 11,338 (0 skipped, 0 with no destination) |
| Typical shapes | 608x448 → 304x224 (half) or 608x448 → 608x448, formats `0x13` and `0x6` |

So the "our EFB copies are missing" hypothesis is **wrong**: copies are emitted and drawn at ~11 per
frame. Whether their *contents* are correct is still unknown, and cannot be settled by readback here.
It needs a real reference image (§7.4), not a probe.

**First timing baseline (Vita3K, emulator only — this is not a console frame rate).** One 1000-frame
window:

| Measure | Value |
|---|---|
| Wall per frame | **151.94 ms** (6.6 fps equivalent) |
| Draws in that frame | 174 |
| Shader programs compiled | 71 |
| Resolve ops per frame | ~11 |

For scale, DuskOnVita measured **154 ms/frame on real hardware** for its first build (6.34 fps in
Link's house, 22 Oct, 333 MHz, everything on one core). Our emulator figure is of the same order as
their *worst* console figure, which is the useful reading: emulator timing here is dominated by
software rendering and by shader compilation, and it should be treated as an upper bound on cost, not
as a prediction. **No FPS claim about our port on hardware can be made from this.**

**Caveat on the probe code that remains.** The `sResolveSeen` / `sResolveDrawn` / `sResolveNoDest`
counters and the 300-frame timing window are still in
[`vita_gl_replay.cpp`](tp-vita/vita/aurora/overlay/gfx/vita_gl_replay.cpp). They are cheap and they
are what produced the table above. Remove them once a reference render exists, along with the
`mch_trace` bring-up tracing.

### 7.7 Render scale: 75%, measured (2026-11-02)

The EFB target is now rendered at 75% and upscaled by `present()` to 960x544. This is a **behaviour
change**: the picture is softer. It is currently a `constexpr` in
[`ensure_efb()`](tp-vita/vita/aurora/overlay/gfx/vita_gl_replay.cpp:745); it should become a switch
before anyone plays this.

**A/B, Vita3K, identical runs, window ending at frame 1080 (71 programs compiled, ~170 draws/frame):**

| EFB scale | Wall per frame | Equivalent | Distinct colours in the screenshot |
|---|---:|---:|---|
| 100% | 151.94 ms | 6.6 fps | 78,360–87,870 |
| 75% | ~113 ms | ~8.8 fps | 80,137–84,554 |

That is a **26% cut in wall time for a 44% cut in fragments**. The gap is the useful finding: the frame
cost is *not* purely fill-bound. Draw submission and the ~11 resolve ops per frame are unchanged by
resolution and dominate proportionally more as the target shrinks. Do not expect a 44% win from
resolution alone, and do not assume more aggressive scaling keeps paying.

**Both numbers are emulator figures and neither is a console frame rate.** Vita3K renders in software
here; the absolute values mean little, the ratio is the transferable part.

### 7.8 Three measurement traps that each produced a wrong conclusion (2026-11-02)

Every one of these cost a session and each is a general lesson.

1. **A "hang" that was a SIGKILL during a buffered write.** Runs appeared to freeze while compiling
   `gx-1a49877e`, with the log line truncated mid-way (`... vertex 1113` and nothing after). The
   conclusion "that shader hangs the compiler" was wrong: the shader compiled in 283 ms in an earlier
   run. [`in_container.sh:25`](tp-vita/tests/vita3k/in_container.sh:25) ends every run with
   `kill $pid; sleep 2; kill -9 $pid`, and a `Log.info` into a pipe is block-buffered, so the last
   line is cut mid-write. **A truncated log line means the process was killed, not that it stalled.**
   Use `Log.warn`/stderr for final-state reporting.
2. **A window that did not fit in the run.** A 300-frame timing window never fired in a 60 s run,
   because shader compilation costs 200–700 ms per program and the first ~70 programs are built
   lazily, so startup alone can consume the whole budget. Windows were cut to 120 frames.
3. **Two blank-frame regressions that read as wins.** Forcing `separatePresentation` produced a
   rock-steady **33.33 ms/frame, 30.0 fps equivalent** — which is the simulation clock pacing an empty
   loop, because the interpolation presentation path (`dusk::interp::begin_presentation` with
   `fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw)`) does not draw on this backend. The
   screenshots were 263 bytes. **Check pixels before believing a frame time**: a blank frame is
   263 bytes, a rendered one is 30 KB–360 KB, and `mode=1` versus `mode=RGB` in the PNG header is the
   cheapest possible assertion.

### 7.9 Vertex decode cache: correct, 74% hit rate, and still not worth it here (2026-11-02)

DuskOnVita's frame profile puts vertex preparation first at **66 ms of a 154 ms frame**, so a decode
cache was the obvious thing to take from them. It is implemented in
[`vita_vertex.cpp`](tp-vita/vita/aurora/overlay/gx/vita_vertex.cpp) and it is **switched off**
(`kVertexCacheEnabled = 0`). This section is the reason, and it is deliberately blunt because the first
version of this cache had a silent correctness bug that would have been very expensive to find later.

#### The first version was unsound, and its 83.8% hit rate was meaningless

Two defects, both found by reading the decoder back rather than by any test:

1. **Wrong buffer.** Indexed attributes read from `frame.storage` (that is `Ctx::arr`), not
   `frame.verts`. The first key computed a size as "vertices + `indexed_array_bytes()`" and then hashed
   and copied that many bytes starting at `frame.verts[vertStart]`. That is an overread past the vertex
   range, and the array bytes the decode actually consumes were never part of the key. Two draws with
   identical index bytes over different snapshotted arrays could **false-hit** and silently render each
   other's positions. A false hit is the worst failure mode a cache has.
2. **Incomplete key.** Decode also depends on the `PipelineConfig` and on `draw.immediateData`
   (`currentPnMtx`, per-attribute `arrayStart`). Identical vertex bytes under two pipelines decode
   differently. Neither was hashed.

The fix builds real `ArrayRange`s for the indexed arrays and folds the per-frame `frame.storage`
digest, the pipeline config and the immediate data into the key, with `memcmp` as the authority on the
vertex bytes:

```cpp
key = config_key(config, draw);
key ^= hash_bytes(src, srcBytes);
for (each indexed array range) { digest = (digest ^ hash_bytes(range)) * FNV_Prime; key ^= digest; }
```

Every hit-rate number taken before this fix is void and was re-taken.

#### Re-measured, sound, same scene (frames 1080–1200, ~165 draws/frame, 71 programs)

| build | decode | hash + copy | total vertex cost | per frame |
|---|---:|---:|---:|---:|
| cache OFF | 474.3 ms | — | 474.3 ms | **3.95 ms** |
| cache ON | 382.9 ms | 177.1 ms | 560.0 ms | **4.67 ms** |
| cache OFF (heavier window) | 773.4 ms | — | 773.4 ms | **6.45 ms** |
| cache ON (same window) | 344.3 ms | 235.8 ms | 580.1 ms | **4.83 ms** |

Hit rate **74–76%**, which is real: the decode work roughly halves. But the entire vertex path is only
**4–6 ms of a ~130 ms frame in Vita3K**, so the change is smaller than the run-to-run noise, and on a
faster host the hash+copy overhead eats the win. Wall-clock A/B was inconclusive by construction
(171–504 ms for the same configuration); this table exists because timing the cache internally gives a
low-noise number that a whole-frame timer cannot.

Also measured and worth knowing: 20k–47k requests per window exceed `kVertexCacheSourceMax = 32768`
and are never cached (`too-big`), and evictions roughly equal misses — the working set is larger than
the 12 MiB budget at this scene.

#### Why it stays off, and what would change that

Vita3K runs guest ARM on a fast x86-64 host, so it **understates** CPU-bound guest work by roughly an
order of magnitude. DuskOnVita's 66 ms was measured on a **real Vita 1000**. If that figure transfers,
this cache is worth 15–45 ms/frame there — a fifth to a third of the frame — and should be turned on.
That is a decision for hardware. The code is kept, correct and behind one constant, precisely so that
test is a one-character change plus a console.

The general lesson, recorded because it applies to every remaining optimisation on this port: **in this
emulator, measure the component, not the frame.** A frame timer here cannot resolve a 1 ms change
because the noise is ±100 ms, and host/guest CPU cost ratios are not transferable.

#### Correction: the intermittent core dump is a guest abort, not an unrelated fault (2026-11-02)

Two Vita3K runs during the work above died with `Undefined instruction at 0x8227A58E` and a
`Trace/breakpoint trap (core dumped)`. That was first written off here as "an unrelated guest fault,
reproducible with the cache compiled out". The second half was true and the first half was wrong.

- `0x8227A58E` is **inside our own image**, not firmware: [`tpv_vita.velf`](tp-vita/build/tpv_vita.velf)
  has `.text` at `0x81000040`..`0x822879D8`, and `build/tpv_vita` resolves that address via
  `arm-vita-eabi-addr2line` to **`_kill_r`**, newlib's syscall wrapper.
- `_kill_r` is what the guest calls when it raises a signal against itself, i.e. `abort()` from an
  assert or a fatal path. `Trace/breakpoint trap` on the host is the emulator tear-down after that.
  So this is **our process choosing to die**, and the aborting call is the thing to find, not an
  emulator bug.
- It is **not** the long-standing `0xBF18265F` allocation failure: [`dvd_trace.txt`](tp-vita/tests)
  from the crashed run ends with successful `JKRExpHeap::create` calls and `parentFree=0x734CA0`
  (7.5 MiB free), with no failing allocation logged.
- What is missing is the reason. `boot.log` was empty and the fatal path evidently does not flush a
  message before `abort()`, so nothing in the surviving artefacts says which assertion fired.

To make the next occurrence explain itself, the cheap and durable move is a custom assert/signal
handler that writes the failing expression, file and line to `ux0:data/tpvita/` **before** dying, plus
`-rdynamic`-style symbolication through the existing `.map`-less build (add `-Wl,-Map=` — there is none
today, which is why naming the caller needed the 666 MB debug ELF). Do not classify a
`Trace/breakpoint trap` as an emulator fault again without resolving the address first; one
`addr2line` would have saved this correction.

### 7.10 The abort now names itself (2026-11-02)

The conclusion of §7.9's correction was that a `Trace/breakpoint trap` was our own `abort()`, and that
nothing recorded which one. This implements the fix, and the fix was verified by deliberately crashing.

#### What was actually wrong

Three things, each found by looking rather than guessing:

1. **Every reachable abort site is silent.** `PPCHalt()` and `OSSwitchStack()`/`OSSwitchFiber()` are a
   bare `abort()` (`dusklight/src/dusk/stubs.cpp`, `OSContext.cpp`), and
   `ConfigVarBase::markRegistered()/unmarkRegistered()` abort on a double (un)register with no message
   at all (`src/dusk/config.cpp`). A double registration is a plausible intermittent trigger for the
   ~1-in-7 death, but it is not provable yet — which is exactly the point.
2. **No log had a critical line.** Checked all 10 surviving game logs: `grep -ciE "\|C\||fatal|assert|
   abort|panic"` returns **0 for every one**. The death never reaches the logger, so no amount of
   log-reading could have explained it.
3. **`boot.log` was empty** because the fatal path does not flush before dying.

#### What was added

- [`vita/src/crash_record.cpp`](tp-vita/vita/src/crash_record.cpp) — `__wrap_abort()` catches **every**
  `abort()` in the process via `-Wl,--wrap=abort`, so dusklight needs no edits and the
  "behaviour comes from the decompilation" posture is intact. It writes the context note, the caller
  address resolved to a symbol, and a SIGABRT handler dump, all flushed line by line to
  `ux0:data/tpvita/crash.log` (previous run kept as `crash.log.1`). Opening the file is deferred to the
  first line, so a clean run leaves nothing behind.
- [`vita/src/crash_symbols.cpp`](tp-vita/vita/src/crash_symbols.cpp) + [`tools/map_symbols.py`](tp-vita/tools/map_symbols.py)
  — the link map is turned into a 36,686-entry table and embedded, so naming an address needs no
  filesystem access and no allocation on the fatal path. There is now a `-Wl,-Map=` too; there was none.

  **Superseded by §7.11**: that table was missing 43.8% of the symbols and every size was 0, so it named
  the wrong function. The table is now generated from the ELF with `nm -S -C` and shipped as a file in
  the VPK; the `-Wl,-Map=` is gone.
- [`tests/host/crash/`](tp-vita/tests/host/crash/run.sh) — 7 assertions against the real generated
  table (resolve, zero-size extent, below-image, truncation, null buffer). All pass. Now 19.

Measured proof, from a deliberately planted trigger:

```
=== abort() from selftest: deliberate abort() from main() ===
abort_caller=0x81009b3d  main
=== SIGABRT ===
last_context=selftest: deliberate abort() from main()
maps: /proc/self/maps unavailable
```

The caller was **named from the embedded map**, which is the capability that was missing: no
`addr2line`, no 666 MB ELF, no assuming link-time equals run-time.

**That name was wrong.** `main` happened to be correct; the next crash reported
`borealis::log::file_descriptor()` for an address inside `write_fatal`. See §7.11.

#### Two honest limits

- **`assert()` is compiled out.** `TPV_GAME_DEFS` sets `NDEBUG=1` (CMakeLists.txt:139) and the binary
  contains **zero** `assert` references. So `__wrap___assert_func` is defensive only and fires on
  nothing today. Deliberately not changed: enabling assertions in a release build of a game with
  inherited asserts would trade one mystery for a new class of deaths. If a build ever defines
  `DEBUG`, the wrapper is already in place.
- **`/proc/self/maps` is unavailable** in Vita3K, so the load base is not recorded. That did not matter
  here because Vita3K evidently maps the ELF at its link address, which the `main` resolution
  demonstrates. On hardware this line only matters if the loader relocates the image.

#### What to do when it fires

`crash.log` now names the aborting function. For the four silent sites above, that is the whole
answer: `PPCHalt` or `OSSwitchFiber` means a stub was reached that this port has not implemented, and
`ConfigVarBase::registerConfigVar` means a double registration, which is a real bug in the config
system rather than a port defect. Fix the caller, not the wrapper.

### 7.11 The recorder named the wrong function, and the fix (2026-11-03)

§7.10 shipped and then lied on its first real crash. The log said:

```
=== abort() from abort() ===
abort_caller=0x81c7f805  borealis::log::file_descriptor()
```

`file_descriptor()` is a one-line atomic load (`return g_fileFd.load(...)`) at `0x81c7f3d8`, 0x10 bytes
long. It cannot abort. `addr2line` against the real ELF put the true site at
`borealis::log::detail::write_fatal()` (`dusklight/extern/borealis/src/log.cpp:646`, the
`write_bootstrap_locked` branch, immediately before `std::abort()` at 654). So the finding was real and
useful — the death is a `LogLevel::Fatal` from borealis — but the name pointing at it was wrong, and a
wrong name is worse than no name because it sends the reader to innocent code.

**My first diagnosis was also wrong**, and it is worth recording because it was plausible and I acted
on it: I concluded the embedded table was "one build stale by construction". Measurement killed that.
Comparing two consecutive tables, **0 of 34,572 symbols changed address**, and the ELF timestamp
matched. Staleness was not the mechanism.

#### The two real defects, both in the generator

1. **It threw away 43.8% of the table.** [`tools/map_symbols.py`](tp-vita/tools/map_symbols.py) rejected
   any name containing a space. C++ demangled names contain spaces *and* commas —
   `write_fatal(std::basic_string_view<char, ...>, ...)` is one — so 31,439 of 71,787 parsed rows were
   silently dropped. `write_fatal` was not in the table at all, so the nearest *surviving* symbol was
   reported instead.
2. **Every size was 0.** ld `-Map` symbol lines carry no size column, so the "is the address actually
   inside this symbol" check could never fire. It was dead code that looked like protection.

`nm -S --defined-only -C` supplies both. It is the right source and it was available all along; the
map was chosen only because the build already produced one.

#### Why the table is now a file, not an embedded blob

A complete text-symbol table is **4.2 MiB**, and `.text`+`.rodata` has **118 KiB** of slack before the
data segment (see `tools/check_text_gap.py`). The old table fit at 1.69 MiB only *because* it had
discarded 43.8% of the symbols. So the table now ships as `app0:data/tpvita/crash_symbols.txt` inside the
VPK — 5.16 MB, 61,749 symbols, opened only while dying. That also removes the staleness class of bug
entirely, because the table is generated from the same ELF that ships, in the same build.

The generator no longer writes C string literals; it writes **real tabs and newlines**. Both forms were
tried and the C-literal form silently produced a table the resolver read as one malformed line.

#### The check that makes a name trustworthy, and the bug it found immediately

The table records the address of `tpv_crash_table_anchor`, a function defined in
[`vita/src/crash_symbols.cpp`](tp-vita/vita/src/crash_symbols.cpp). Validity is that recorded address
matching the live one. When they disagree, the recorder prints addresses only and says so.

That check ran on its first boot and **failed on a table that was actually correct**:

```
crash symbols: MISMATCH, table 0x8108f570 vs live 0x8108f571
```

Off by exactly 1: the ARM **Thumb bit**. A function pointer has bit 0 set; the ELF symbol value that
`nm` prints has it clear. Both are now normalised before comparing. This is the argument for running
the check on every boot instead of only during a crash — a check you can only run when something has
already gone wrong cannot be validated at all.

#### Verified, in Vita3K, end to end

Every boot now logs one line, so the capability is confirmed without waiting for a rare bug:

```
[   0.061] crash symbols: table matches this build (anchor 0x8108f570); crash logs will name
```

A deliberate abort (`ux0:data/tpvita/selftest-abort`, a permanent opt-in trigger that deletes itself)
produced:

```
=== abort() from crash self-test (ux0:data/tpvita/selftest-abort) ===
abort_caller=0x81009bab  main
symbols: table matches this build
```

and `main` really does span `0x81008560`+`0x1814` = `0x81009d74`, so `0x81009baa` is inside it. The
name is true.

The specific regression that caused the misnaming is now pinned in the host test: an address in the
middle of `write_fatal` resolves to `write_fatal`, and an address past the end of `file_descriptor()`
resolves to the *next* symbol rather than to `file_descriptor()`.

[`tests/host/crash/`](tp-vita/tests/host/crash/run.sh) is 19 assertions, all passing, against the real
generated table. They read addresses out of the table instead of hardcoding them, because hardcoded
addresses test the build rather than the resolver.

#### The lesson, stated plainly

The recorder was built to remove an unknown and instead introduced a confident wrong answer, which is
strictly worse. Three things would each have caught it: assert on the artefact (43.8% of rows dropped
is visible in a row count), use a source that carries the field you are checking (sizes), and make the
tool state its own reliability rather than assuming it. Only the third was in the original design, and
it was defeated by a bug one layer down.
