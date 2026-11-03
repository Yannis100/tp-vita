# Running the VPK in Vita3K (headless)

`run.sh` installs and starts `build/tpv_vita.vpk` in the official Linux build of [Vita3K](https://vita3k.org/), inside a podman
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
That is a reproducible *sequence*, not a reproducible *run*. A frame-counted injector would need a hook
inside the app, not an `xdotool` driver.

What it found the first time it was used: the port stops at a GameCube memory-card prompt ("No Memory
Card found in Slot A") in a self-perpetuating loop, so the game cannot be played past file select. See
`HANDOVER.md` §3d.

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

## What it does not prove

- **Vita3K is an emulator, not a Vita.** The run stops at the diagnosis screen: with no real disc the app never starts the game, so no GX
  frame, no vitaGL, no shader compile (`libshacccg.suprx`), no audio and no controller beyond one key were exercised.
- Memory budget, timing, file-system permissions and the SGX543 are not modelled. The boot log's "firmware" and "argv[0]" lines are empty here
  because Vita3K does not implement `sceKernelGetSystemSwVersion` and passes no argv; they are filled on hardware (unchecked).
- Vita3K's `config.yml` keys are version-specific; `config.yml` here matches the version above.
