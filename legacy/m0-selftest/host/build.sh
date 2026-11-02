#!/bin/sh
# Build and run the TP Tier-1 game-logic self-test on the host (Linux).
# Exits 0 = all tests passed. Requires gcc/g++.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TP="${TP_DEPS:-$HERE/../dusklight}"
CC=gcc
CXX=g++
COMMON="-std=gnu11 -O2 -I $HERE/host -I $HERE/vita/include -I $TP/include -I $TP/libs/revolution/include -I $TP/extern/aurora/include"
OUT="$HERE/build-host"
mkdir -p "$OUT"

$CC $COMMON -c "$HERE/vita/stubs/mtx.c" -o "$OUT/mtx.o"
$CC $COMMON -c "$HERE/vita/stubs/ppc_math.c" -o "$OUT/ppc_math.o"
$CC -std=gnu11 -O2 -I $HERE/host -I $HERE/vita/include -c "$HERE/vita/vita_runtime.c" -o "$OUT/vita_runtime.o"

$CXX -std=gnu++20 -O2 -I $HERE/host -I $HERE/vita/include -I $HERE/src \
  -I $TP/include -I $TP/libs/JSystem/include -I $TP/libs/revolution/include \
  -I $TP/extern/aurora/include -include "$HERE/vita/include/global.h" \
  -DTARGET_PC=1 -D__VITA_PORT__=1 \
  "$HERE/src/main.cpp" \
  "$HERE/vita/stubs/os.cpp" \
  "$HERE/vita/stubs/jsupport_fix.cpp" \
  "$TP/libs/JSystem/src/JMath/JMath.cpp" \
  "$TP/libs/JSystem/src/JMath/random.cpp" \
  "$TP/libs/JSystem/src/JMath/JMATrigonometric.cpp" \
  "$TP/libs/JSystem/src/JSupport/JSUList.cpp" \
  "$TP/libs/JSystem/src/JSupport/JSUInputStream.cpp" \
  "$TP/libs/JSystem/src/JSupport/JSUOutputStream.cpp" \
  "$TP/libs/JSystem/src/JSupport/JSUMemoryStream.cpp" \
  "$OUT/mtx.o" "$OUT/ppc_math.o" "$OUT/vita_runtime.o" \
  -o "$OUT/tp_selftest"

"$OUT/tp_selftest"
