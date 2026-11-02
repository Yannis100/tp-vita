#!/bin/sh
# Runs INSIDE the container (see run.sh): Xvfb + Vita3K (SDL frontend, OpenGL on llvmpipe) installing and starting the VPK,
# screenshots in /work/out. Usage: in_container.sh <seconds-between-shots> <shots> [key ...]
# With keys: after the shots, each key is pressed (2 s apart) until the app exits; the key that ended it is printed.
interval=${1:-15}; shots=${2:-2}; shift 2 2>/dev/null
rm -f /work/out/shot_*.png /work/out/vita3k.log
Xvfb :99 -screen 0 1280x800x24 >/work/out/xvfb.log 2>&1 &
sleep 2
export DISPLAY=:99 QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy
cd /opt/vita3k
./Vita3K -c /work/cfg/config.yml --frontend sdl -w /vpk/app.vpk >/work/out/vita3k.log 2>&1 &
pid=$!
i=1
while [ $i -le "$shots" ]; do
  sleep "$interval"
  import -window root /work/out/shot_$i.png 2>/dev/null
  kill -0 $pid 2>/dev/null || { echo "Vita3K exited before shot $i"; break; }
  i=$((i + 1))
done
for k in "$@"; do
  kill -0 $pid 2>/dev/null || break
  xdotool keydown "$k"; sleep 0.3; xdotool keyup "$k"; sleep 2 # held: a 12 ms tap can fall between two polls of the app or of Vita3K
  kill -0 $pid 2>/dev/null || echo "app exited after key: $k"
done
if kill -0 $pid 2>/dev/null; then kill $pid; sleep 2; kill -9 $pid 2>/dev/null; echo "stopped (still running)"; fi
echo done
