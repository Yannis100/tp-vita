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

**The game boots, reaches its main loop, and renders.** That is a large milestone and it is
verified from emulator artefacts, not assumed.

Verified as of this handover:

- Build is clean: `cmake --build build -j` exits 0, produces `build/tpv_vita.vpk`,
  text→data gap 105,200 bytes.
- `mDoMch_Create()` returns 1 (previously it never returned).
- `main01()` enters its `do { ... } while (dusk::IsRunning)` loop and keeps iterating.
- Vita3K renders real frames: after the §7.3 texture fix, screenshots carry 78,360–87,870 distinct
  colours each (before it: 1,614–3,979, i.e. untextured), with content
  bounding box `(271,128)–(1008,671)` on a 1280×800 canvas.
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
The screenshots show a rendered 3D scene, but nobody has confirmed it is the *correct* scene
(no reference comparison against a real GameCube or the desktop build has been done). Audio is
disabled (`DUSK_AUDIO_DISABLED`). Card saves, pause menu, and most of the game's content have
never been exercised. Treat "boots and renders" as the accurate claim, not "playable".

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
cmake --build build -j"$(nproc)"
```

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

### 7.4 Not started

- Real hardware testing (nothing has run outside Vita3K).
- Audio (currently `DUSK_AUDIO_DISABLED`; the audio engine is told it initialised, which is a lie
  to get past boot).
- **A reference render.** This is now the highest-value item: build dusklight for desktop
  (WebGPU/Dawn), boot the same `GZ2P01.iso`, and capture the same first scene. Without ground truth
  every visual judgement here — including the missing cool tones above — is a guess.
- Card saves, pause menu, gameplay content.

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

**Do the reference desktop render (§7.4).** It is the only item that converts the rest of this
document from "the geometry is right" into "the picture is right", and it is the one artefact that
two independent ports are both blocked on: DuskOnVita's open bug is texture flicker and its own ask
is a matched Dolphin capture of the same scene (§7.5). Build dusklight for desktop (WebGPU/Dawn),
boot the same `GZ2P01.iso`, capture the same first scene, and compare against the Vita3K frame. That
single comparison settles the open questions about the missing cool tones (§7.3) and gives a target
for every later rendering fix.

**Do not spend more time on frame-rate micro-optimisations in Vita3K.** §7.8 and §7.9 both ended with
the same answer: this emulator cannot resolve a change worth 1-2 ms because its own noise is ±100 ms,
and it runs guest ARM on a host roughly an order of magnitude faster than a Cortex-A9, so CPU gains
measured here do not transfer. The vertex cache in §7.9 is the worked example: correct, 74% hit rate,
halves the decode work, and still not a measurable win on the emulator. Anything in this list that is
pure CPU (vertex cache, static geometry by reference, the GX worker thread) belongs on hardware.
Something that reduces **GPU** work or **draw count** is more likely to show up even here.

Second, if a run dies: §7.1's size arithmetic is now sane (the arena is 24 MiB larger, §7.1), so
watch whether the `0xBF18265F` failure rate changes at all before doing anything else with it. The
instrumentation already records `caller=%p`.

For reference, the honest position as of this revision: the port boots, reaches the main loop and
renders correctly-shaped frames with plausible colour counts on an emulator. **A separate port does
the same thing on real hardware at 12-28 FPS with working saves and input (§7.5), so feasibility is
no longer in question.** What remains unproven here is scene correctness (no reference image),
performance, audio, and hardware behaviour — plus one intermittent allocation failure in roughly 1
run in 7 whose cause is not yet known.

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
