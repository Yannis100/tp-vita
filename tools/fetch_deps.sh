#!/usr/bin/env bash
# Fetch the third-party sources Dusklight/Aurora expect, at the versions Dusklight pins.
# They are compiled for arm-vita-eabi by tp-vita's CMake (nothing here runs at fetch time).
set -euo pipefail
DEST="${1:-$(cd "$(dirname "$0")/.." && pwd)/deps}"
mkdir -p "$DEST/_dl"

fetch() { # name url [strip]
  local name="$1" url="$2" strip="${3:-1}"
  [ -d "$DEST/$name" ] && { echo "have $name"; return; }
  local file="$DEST/_dl/$(basename "$url")"
  [ -f "$file" ] || curl -fL --retry 3 -o "$file" "$url"
  mkdir -p "$DEST/$name"
  case "$file" in
    *.zip) local tmp; tmp="$(mktemp -d)"; unzip -q "$file" -d "$tmp"
           if [ "$strip" = 1 ]; then mv "$tmp"/*/* "$DEST/$name/"; else mv "$tmp"/* "$DEST/$name/"; fi
           rm -rf "$tmp" ;;
    *) tar -xf "$file" -C "$DEST/$name" --strip-components="$strip" ;;
  esac
  echo "got $name"
}

fetch tracy    https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.zip
fetch xxhash   https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
fetch abseil   https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz
fetch json     https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
fetch cxxopts  https://github.com/jarro2783/cxxopts/archive/refs/tags/v3.3.1.tar.gz
fetch picosha2 https://github.com/okdshin/PicoSHA2/archive/refs/tags/v1.0.1.tar.gz
fetch miniz    https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip 0
fetch imgui    https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
fetch rmlui    https://github.com/encounter/RmlUi/archive/f00a0fee38391b2f927114e11bea18dc0a7dba1e.tar.gz
fetch sqlite3  https://sqlite.org/2026/sqlite-amalgamation-3510300.zip

# Vita patches for fetched sources (idempotent).
PATCHES="$(cd "$(dirname "$0")/.." && pwd)/patches"
apply_patch() { # dir patch
  if patch -d "$DEST/$1" -p1 --dry-run -R -s -f < "$PATCHES/$2" >/dev/null 2>&1; then
    echo "patched $1 ($2)"
  else
    patch -d "$DEST/$1" -p1 -s < "$PATCHES/$2" && echo "applied $2"
  fi
}
apply_patch abseil abseil-vita.patch
