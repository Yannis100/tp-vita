#!/bin/sh
# Re-assemble build/aurora_lib from upstream + overlay + patches, then syntax-check one or more
# merged-tree sources for arm-vita-eabi (no linking, no cmake needed):
#   tools/check_overlay.sh gx/vita_shader.cpp [gfx/vita_texture.cpp ...]
# Paths are relative to build/aurora_lib (or start with vita/ for files of this repo, or are absolute). Exit status is non-zero if any file fails.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
aurora=${AURORA_DIR:-$here/../dusklight/extern/aurora}
python3 "$here/tools/build_aurora_tree.py" "$aurora/lib" \
  "$here/vita/aurora/overlay" "$here/vita/aurora/patches.py" "$here/build/aurora_lib" >/dev/null
flags=$(cat /tmp/tpv/aurora_flags.txt 2>/dev/null)
rc=0
for f in "$@"; do
  echo "== $f"
  case "$f" in /*) src=$f ;; vita/*|tools/*) src=$here/$f ;; *) src=$here/build/aurora_lib/$f ;; esac
  # shellcheck disable=SC2086
  arm-vita-eabi-g++ -std=gnu++20 -fsyntax-only -fsigned-char -fno-short-enums -Wall -Wno-unused-function -I"$here/deps/imgui" -DIMGUI_USER_CONFIG='"aurora/imgui_config.h"' -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS $flags \
    -I"$here/build/aurora_lib" "$src" || rc=1
done
exit $rc
