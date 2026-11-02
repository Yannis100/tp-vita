#!/bin/sh
# Fails if Vita-side code uses a printf length modifier that the VitaSDK's newlib does not implement.
#   tests/host/check_printf_formats.sh [dir...]      (default: vita/)
# Found by running the VPK in Vita3K: the boot log printed "zu disc-like file(s)" for  log.line("%zu", s.files.size())  - %z is
# not supported by the newlib in the VitaSDK, the host glibc (which every host test uses) accepts it, so no host test could
# see it. %ll (long long) IS supported on the Vita: the 1459978240-byte size printed correctly in the same run.
# Cast to unsigned / unsigned long long (or use PRIu32 / PRIu64) instead of %zu / %zd / %ju / %jd / %td / %hh*.
root=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || set -- "$root/vita"
bad=$(grep -rnE --include='*.c' --include='*.cc' --include='*.cpp' --include='*.h' --include='*.hpp' \
  '%[-+ #0-9.*]*(z|j|t|hh)[diouxX]' "$@" || true)
if [ -n "$bad" ]; then
  echo "printf length modifiers the Vita's newlib does not implement (use unsigned / unsigned long long casts):"
  echo "$bad"
  exit 1
fi
echo "printf formats: no %z / %j / %t / %hh in $*"
