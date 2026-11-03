#!/bin/sh
# Runs the built VPK in Vita3K, headless, inside a podman container. Nothing is installed on the host.
#   tests/vita3k/run.sh image                         build the container image (once)
#   tests/vita3k/run.sh firmware /path/PSVUPDAT.PUP   install YOUR OWN Sony firmware into the work directory (once)
#   tests/vita3k/run.sh run [interval] [shots] [key ...]   install + start the VPK; screenshots in $work/out/shot_N.png,
#                                                     the emulator log in $work/out/vita3k.log; keys are pressed afterwards
#                                                     (x = Cross in Vita3K's default keyboard map)
# Environment:
#   VITA3K_DIR   the extracted official Vita3K Linux build (the folder with the Vita3K binary)   [required]
#   TPV_V3K_WORK work directory: emulator data (firmware, ux0/), config, outputs   [default $HOME/vita3k-work]
#   TPV_VPK      the VPK to run   [default build/tpv_vita.vpk]
# What it shows: that the VPK installs, the eboot loads and links against the firmware modules, and what the app writes to ux0:data/tpvita/
# (boot.log) and puts on the screen. Vita3K is an emulator: this is not a hardware test (no vitaGL / libshacccg.suprx behaviour is checked).
# Neither the firmware nor the Vita3K binary belongs to this repository; do not put them in the VPK or the repository.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${TPV_V3K_WORK:-$HOME/vita3k-work}
# Default to the dev VPK when it exists: this harness is for reproducing failures, and a dev build
# names its own variant in the log. Set TPV_VPK to run a specific one (the release build is
# build/tpv_vita-release.vpk).
if [ -n "${TPV_VPK:-}" ]; then
  vpk=$TPV_VPK
elif [ -f "$root/build/tpv_vita-dev.vpk" ]; then
  vpk=$root/build/tpv_vita-dev.vpk
elif [ -f "$root/build/tpv_vita-release.vpk" ]; then
  vpk=$root/build/tpv_vita-release.vpk
else
  vpk=$root/build/tpv_vita.vpk
fi
image=localhost/tpv-vita3k-env
cmd=${1:-}; [ $# -gt 0 ] && shift
# the preference directory must exist before Vita3K starts: with a missing one --firmware aborts ("cannot create directories: Invalid argument")
mkdir -p "$work/home/.local/share/Vita3K/Vita3K" "$work/out" "$work/cfg"
[ -f "$work/cfg/config.yml" ] || cp "$here/config.yml" "$work/cfg/config.yml"
need_dir() { [ -x "${VITA3K_DIR:-}/Vita3K" ] || { echo "set VITA3K_DIR to the extracted Vita3K Linux build" >&2; exit 2; }; }
common="-v $work/home:/work/home:rw -v $work/cfg:/work/cfg:rw -v $work/out:/work/out:rw -e HOME=/work/home"
case "$cmd" in
  image) podman build -t "$image" "$here" ;;
  firmware)
    need_dir; pup=${1:?usage: run.sh firmware /path/to/PSVUPDAT.PUP}
    podman run --rm -v "$VITA3K_DIR:/opt/vita3k:ro" -v "$pup:/fw/fw.pup:ro" $common -e QT_QPA_PLATFORM=offscreen "$image" \
      sh -c 'cd /opt/vita3k && ./Vita3K -c /work/cfg/config.yml --firmware /fw/fw.pup 2>&1 | tail -5' ;;
  run)
    need_dir; [ -f "$vpk" ] || { echo "no VPK at $vpk (build first)" >&2; exit 2; }
    interval=${1:-15}; shots=${2:-2}; if [ $# -ge 2 ]; then shift 2; else shift $#; fi # the rest are keys
    podman run --rm -v "$VITA3K_DIR:/opt/vita3k:ro" -v "$vpk:/vpk/app.vpk:ro" -v "$here/in_container.sh:/work/in_container.sh:ro" \
      $common "$image" sh /work/in_container.sh "$interval" "$shots" "$@" ;;
  *) sed -n 2,16p "$0"; exit 1 ;;
esac
