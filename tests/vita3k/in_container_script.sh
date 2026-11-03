#!/bin/sh
# Runs INSIDE the container (see run.sh). Same Xvfb + Vita3K setup as in_container.sh, but instead of
# "sleep N, screenshot, kill", it replays a fixed action script so the game is actually DRIVEN past the
# first screen. Why this exists: an idle run never loads new scene data, so it can only reach the
# fatal sites on the boot path. The data-dependent ones (gx::comp_cnt_count / comp_size on an
# unsupported vertex attribute format, dl.cpp:62, command_processor.cpp:366) need new geometry.
#
# Usage: in_container_script.sh <script-file> <shot-prefix>
#
# Script grammar, one command per line, '#' starts a comment:
#   wait  <seconds>        sleep
#   tap   <key>            keydown, 0.12 s, keyup   (a real tap; see the note below)
#   hold  <key> <seconds>  keydown, sleep, keyup
#   shot  <name>           screenshot to /work/out/<prefix><name>.png
#
# HONEST LIMIT: the sequence of keys is fixed, the TIMING is wall-clock (sleep), not frame-counted.
# The emulator's speed varies between runs, so a given press can land on a different frame each time.
# That makes this a reproducible *key sequence*, not a reproducible *run*. It is still far better than
# random input, which changes the test itself every time. A frame-counted injector would need a hook
# inside the app, not an xdotool driver.
set -e
script=${1:?usage: in_container_script.sh <script-file> <shot-prefix>}
prefix=${2:-shot_}
[ -f "$script" ] || { echo "no script at $script" >&2; exit 2; }

rm -f /work/out/${prefix}*.png /work/out/vita3k.log
Xvfb :99 -screen 0 1280x800x24 >/work/out/xvfb.log 2>&1 &
sleep 2
export DISPLAY=:99 QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy

cd /opt/vita3k
./Vita3K -c /work/cfg/config.yml --frontend sdl -w /vpk/app.vpk >/work/out/vita3k.log 2>&1 &
pid=$!
alive() { kill -0 $pid 2>/dev/null; }

# Give Vita3K time to install and start the app before the first scripted action, otherwise early
# presses land on the emulator's own UI and are lost.
sleep 20
if ! alive; then echo "Vita3K exited during startup"; exit 1; fi

n=0
while read -r cmd a b; do
  case "$cmd" in
    ''|\#*) continue ;;
    wait)
      sleep "$a" ;;
    tap)
      alive || { echo "app exited before tap $a"; break; }
      xdotool keydown "$a"; sleep 0.12; xdotool keyup "$a"; sleep 0.4 ;;
    hold)
      alive || { echo "app exited before hold $a"; break; }
      xdotool keydown "$a"; sleep "$b"; xdotool keyup "$a"; sleep 0.3 ;;
    shot)
      n=$((n + 1))
      import -window root "/work/out/${prefix}${n}_${a}.png" 2>/dev/null || true ;;
    *)
      echo "unknown script command: $cmd $a $b" >&2 ;;
  esac
  if ! alive; then echo "app exited after: $cmd $a $b"; break; fi
done < "$script"

if alive; then kill $pid; sleep 2; kill -9 $pid 2>/dev/null; echo "stopped (still running)"; fi
echo done
