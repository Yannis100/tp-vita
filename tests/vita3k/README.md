# Running the VPK in Vita3K (headless)

`run.sh` installs and starts a VPK (by default the dev build, else `build/tpv_vita-release.vpk`; override with `TPV_VPK=`) in the official Linux build of [Vita3K](https://vita3k.org/), inside a podman
container with a virtual display (Xvfb) and software OpenGL (Mesa llvmpipe), and takes screenshots. Nothing is installed on the host.
The container exists because the official Vita3K Linux builds need a very recent glibc and Qt6.

```sh
export VITA3K_DIR=/path/to/extracted/vita3k        # the folder that contains the Vita3K binary (not part of this repository)
tests/vita3k/run.sh image                           # once: build the container image from Containerfile
tests/vita3k/run.sh firmware /path/to/PSVUPDAT.PUP  # once, optional: install your own firmware
tests/vita3k/run.sh run 15 2 x                      # start the VPK, screenshots after 15 s and 30 s, then hold x (Cross) for 0.3 s
tests/vita3k/run.sh script tests/vita3k/scripts/opening.txt   # replay a fixed action script instead
# results: $TPV_V3K_WORK (default ~/vita3k-work): out/shot_N.png, out/vita3k.log, home/.local/share/Vita3K/Vita3K/ux0/data/tpvita/boot.log
```

## Driving the game instead of watching the title screen

`run` never touches the pad until after the last screenshot, so an idle run stays in the title/attract
loop and never loads new scene data. `script` replays a fixed sequence of taps and held directions
(`in_container_script.sh`, which also documents the grammar), taking screenshots at the points the
script asks for.

Its honest limit is written into its header: the **key sequence is fixed, the timing is not**. Presses
are wall-clock, so the emulator's variable speed means a press can land on a different frame each run.
That is a reproducible *sequence*, not a reproducible *run*.

## Frame-counted input (preferred, 2026-11-05)

The limit above is now solved inside the app instead of by an `xdotool` driver: put a script at
`ux0:data/tpvita/input_script.txt` and the game reads it one step per frame. See
[`vita/src/input_script.cpp`](../../vita/src/input_script.cpp); the format is `<frames> <name>[,<name>...]`
with `.` for "hold nothing". It is off when the file is absent and it deletes the file after reading it,
so it can never leak into a manual run.

```sh
w="$HOME/vita3k-work/home/.local/share/Vita3K/Vita3K/ux0/data/tpvita"
cp tests/vita3k/scripts/frame_card_sweep.txt "$w/input_script.txt"
VITA3K_DIR=... TPV_VPK=build/tpv_vita-release.vpk \
  tests/vita3k/run.sh script tests/vita3k/scripts/waits_sweep.txt sw_
cat "$w/input_script.log"      # every step with the frame it fired on
```

Pair it with a `waits_*.txt` harness script, not a key-pressing one: when the app is driven by the
injector, a host key press is noise that can change the result.

Two things to know before writing a script:

- **`STICKLEFT` is not `LEFT`.** Menus in this game read the analog stick
  (`STControl::checkLeftTrigger()`), not the D-pad. Pressing the D-pad does nothing on them.
- **A single press at an exact frame is the wrong shape for a menu.** The frame number is exact, the
  SCREEN at that frame is not: the card prompt is a state machine whose states run variable-length
  animations. The scripts here sweep (alternate a cursor move with `A`) so that every branch either
  advances or re-enters the sweep.

What driving it found: the port used to stop at a GameCube memory-card prompt in a self-perpetuating
loop. With frame-counted input the prompt is answerable, "Yes" creates a real 32,832-byte GCI, and the
game reaches the new-game naming screens. See `HANDOVER.md` §3d.2 and §3d.5.

To test the disc diagnosis, put files in `<work>/home/.local/share/Vita3K/Vita3K/ux0/data/tpvita/` (the Vita's `ux0:data/tpvita/`) before `run`.
Fixtures that are made of your own bytes work: a 1088-byte `GZ2E01.iso` (game ID, then the GameCube magic `C2 33 9F 3D` at offset 0x1C)
produces the "disc image cannot be used / copy was cut short" screen; a `GALE01.iso` and a `sample.rvz` (starts with `RVZ\x01`) produce the
"no disc image found" list with a reason per file. Do not put a real disc image or any Sony file into the repository or the VPK.

## What a run proves (2026-10-27, Vita3K v0.2.1 build 4236, firmware 3.74, OpenGL on llvmpipe)

- The VPK installs and starts; `eboot.bin` loads and links (Vita3K logs "Linking SELF app0:eboot.bin..." and then the app runs). The app also ran
  without firmware installed and wrote the same `boot.log`.
- The first-boot path works on a display: `boot.log` is written in `ux0:data/tpvita/` (and the previous one is kept as `boot.log.1`), the
  screens render with the intended text, colours, wrapping and paths, and **O or X exits with code 0** (`x` is Cross in Vita3K's default keyboard map;
  hold it: the screen polls every 16 ms and a 12 ms `xdotool key` tap was missed 3 times out of 3 in one directory).
  The three screens seen: no disc image; disc image cannot be used (short copy hint, sizes); wrong game / unsupported container list.
- It found a bug no host test could: the boot log read `zu disc-like file(s)` because the VitaSDK's newlib has no `%z` length modifier
  (`%ll` works). Fixed; `tests/host/check_printf_formats.sh` guards it.
- The firmware is not needed to reach these screens (without it Vita3K only logs the missing kernel modules).

## Later state (2026-11-09 onward)

The paragraph below was written when the port still stopped at the diagnosis screen. It no longer does: with a
real disc image the app boots the game, `libshacccg.suprx` is found at `ur0:data/libshacccg.suprx`, GX frames
render, and a frame-counted scripted run reaches in-game gameplay. Saved evidence:
`screenshots/r1488_gameplay_ordon.png`, `screenshots/r1488_intro_narration.png`. Audio is still disabled and
save *reload* is still untested. See `HANDOVER.md` §3d.6 and §7.4c.

## What it does not prove

- **Vita3K is an emulator, not a Vita.** With no real disc the app never starts the game, so no GX frame, no
  vitaGL, no shader compile, no audio and no controller beyond one key are exercised. Even with a disc, memory
  budget, timing, permissions and the SGX543 are not modelled.
- Memory budget, timing, file-system permissions and the SGX543 are not modelled. The boot log's "firmware" and "argv[0]" lines are empty here
  because Vita3K does not implement `sceKernelGetSystemSwVersion` and passes no argv; they are filled on hardware (unchecked).
- Vita3K's `config.yml` keys are version-specific; `config.yml` here matches the version above.
