#!/bin/sh
# Host tests for what main() does before the game starts:  tests/host/disc/run.sh
#   test_probe  disc-image discovery (vita/src/disc_probe.hpp)
#   test_diag   first-boot diagnosis: font, text layout, disc scan, error screens, boot log
#               (vita/src/boot_{font,text,diag}.hpp, and gfx/vita_shader_dump.hpp from the GL replay)
# TPV_SCREEN_DIR=<dir> also writes the error screens as PPM files; TPV_DUMP_FONT=1 prints the font.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
outdir=${TPV_TEST_OUT:-/tmp/tpv/hostgl}
mkdir -p "$outdir"
rc=0
# the Vita newlib has no %z / %j / %t / %hh: found by running the VPK in Vita3K (see the script)
sh "$root/tests/host/check_printf_formats.sh" || rc=1
for t in test_probe test_diag; do
  g++ -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$root/vita/src" -I"$root/vita/aurora/overlay/gfx" "$here/$t.cpp" -o "$outdir/$t"
  echo "== $t"
  "$outdir/$t" || rc=1
done
exit $rc
