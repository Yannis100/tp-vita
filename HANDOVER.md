# Handover: Twilight Princess on PS Vita

**Written:** 2026-10-29
**Updated:** 2026-11-02 — §7.2 (BMD null deref) root-caused and fixed; §7.3 (every texture upload
failing) root-caused and fixed, which raised rendered colour counts ~20-28x; traps 12-15 added in
§8; dusklight uncommitted-file count is now 14. §7.1 is still open and unchanged. The next highest
value item is a reference desktop render (§7.4).
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

- Build is clean: `cmake --build build -j` exits 0, produces `build/tpv_vita.vpk`
  (15,886,724 bytes), text→data gap 111,956 bytes.
- `mDoMch_Create()` returns 1 (previously it never returned).
- `main01()` enters its `do { ... } while (dusk::IsRunning)` loop and keeps iterating.
- Vita3K renders real frames: after the §7.3 texture fix, screenshots carry 78,360–87,870 distinct
  colours each (before it: 1,614–3,979, i.e. untextured), with content
  bounding box `(271,128)–(1008,671)` on a 1280×800 canvas.
- Zero failing allocations, zero crash markers during a clean run.
- All host test suites pass (numbers in §6).

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

### 7.1 (Priority) Intermittent garbage-size allocation — undiagnosed

One run in seven failed with:

```
JKRExpHeap: do_alloc FAILING size=0xBF18265F align=32 heap=archiveHeap
            heapsize=0x8DFF50 used=0x6B4678 free=0x22B794
```

`0xBF18265F` is 3.2 GiB against a heap with 2.27 MiB free — a **corrupted size argument, not
exhaustion**. It is a different fault from the two heap bugs in §4.4.

What is ruled out, by arithmetic rather than guesswork:

- [`daMP_THPPlayerCalcNeedMemory()`](dusklight/src/d/actor/d_a_movie_player.cpp:3673) — every term
  is `ALIGN_NEXT(..., 32)`, so its result is always a multiple of 32. The observed value has
  `& 0x1F == 0x1F`. **Not the source.**
- The room expand buffer at [`d_s_room.cpp:494`](dusklight/src/d/d_s_room.cpp:494) is plain data
  and not obviously culpable, but also not cleared.

Ruled out as non-signature: `0xBF18265F` as IEEE-754 is ≈ -0.594, which looked promising as a
float-as-size bug, but the value is not 32-aligned so it does not come from a 32-aligned
calculator.

**Instrumentation is in place.** The failure path in `JKRExpHeap::do_alloc` records
`__builtin_return_address(0)`, so if it recurs the trace will name the caller:

```
exp_trace("do_alloc FAILING size=0x%X align=%d heap=%s heapsize=0x%X used=0x%X free=0x%X "
          "caller=%p", ...);
```

Six consecutive trials afterwards were clean, so it is genuinely intermittent. My **guess**
(labelled a guess) is a race: it appeared once in seven runs and vanished for six, and the
plausible actors during room loading are the DVD worker thread and the tracker allocator. Do not
act on that without a reproduction.

**Next step:** loop the emulator run until it reproduces, then symbolise `caller=%p` against the
ELF. `arm-vita-eabi-addr2line -e build/tpv_vita` with the guest address.

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

**What remains unproven:** whether these were the reads reported earlier. The original report lost
the faulting instruction, and neither address can be re-symbolised now. The null-deref path is real
and reachable and is now closed, but treat "§7.2 is fixed" as "the one reachable null dereference
in this area is fixed", not as "the earlier log is explained".

Note that `do_alloc` on `TARGET_PC` **aborts** on failure, so if these reads are the same defect as
§7.1 they will present as `SIGABRT`, not as a graceful failure.

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

---

## 8. Traps and gotchas

1. **`tp-vita/` has no git.** Before any sweeping change, snapshot it:
   `tar -czf ../snapshot-$(date +%Y%m%d)/aurora_lib-merged.tar.gz -C build aurora_lib`.
   There is a precedent at `.snapshot-pre-aurora-upgrade-20261029/` which contains
   `aurora_lib-merged.tar.gz`, `dusklight-local-changes.patch`, `dusklight-status.txt` and a
   known-good `tpv_vita.known-good.vpk`. **Do the same for any large refactor.**
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

---

## 9. Files the port owns (`tp-vita/`)

Structure, so you know where to look:

```
tp-vita/
├── CMakeLists.txt              top-level build; text-gap check; VPK packaging
├── PLAN.md                     route to a playable port; milestones M1..M5
├── README.md                   build, install, run instructions
├── HANDOVER.md                 this file
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

Reproduce §7.1. It is now the only known defect that kills a run, and it is already instrumented
(the failure path records `caller=%p`). §7.2 no longer needs to be folded into it — that null
dereference was found by static analysis and closed (see §7.2). Everything else on the board is
either long-running work (hardware testing, audio) or optional polish.

If it will not reproduce, the honest position is: the port boots, reaches the main loop and renders
correctly-shaped frames on an emulator, with one unexplained intermittent crash in roughly 1 run
in 7.
