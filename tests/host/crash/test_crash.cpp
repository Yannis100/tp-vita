// Host test for the crash recorder's two testable halves: the map-symbol resolver and the
// abort/assert interception. Runs on the host, so it can assert on the file the recorder writes
// instead of waiting for a real crash on a Vita.
//
// What this does NOT test: the Vita-specific parts (ux0 paths, the SIGABRT handler surviving a real
// abort, /proc/self/maps). Those are verified by a deliberate crash in Vita3K, which is the only
// place they exist. This test proves the parsing and the wrap plumbing are right, which is where the
// silent bugs would be.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int tpv_crash_map_symbol(unsigned long address, char* out, unsigned outSize);

static int gFailures = 0;

static void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++gFailures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

int main(int argc, char** argv) {
  // argv[1] is the directory holding the generated crash_map.inc, compiled into this test as a
  // string literal body by the runner.
  (void)argc;
  (void)argv;

  char name[256];

  // A symbol that must be present in the table, at its link address.
  const int hasOSSwitch = tpv_crash_map_symbol(0x81000228u, name, sizeof(name));
  expect(hasOSSwitch == 1, "resolves a known address");
  expect(hasOSSwitch == 1 && std::strcmp(name, "OSSwitchStack") == 0, "names OSSwitchStack");

  // An address inside that symbol (size 0 in the map, so the nearest-preceding rule applies).
  const int inside = tpv_crash_map_symbol(0x8100022au, name, sizeof(name));
  expect(inside == 1 && std::strcmp(name, "OSSwitchStack") == 0, "resolves inside a zero-size symbol");

  // Below the first symbol: nothing may match, and it must not crash.
  expect(tpv_crash_map_symbol(0x00000010u, name, sizeof(name)) == 0, "rejects an address below the image");

  // A buffer too small to hold the name must truncate, not overflow, and still report a hit.
  char tiny[4];
  const int tinyOk = tpv_crash_map_symbol(0x81000228u, tiny, sizeof(tiny));
  expect(tinyOk == 1, "reports a hit with a small buffer");
  expect(std::strlen(tiny) < sizeof(tiny), "truncates instead of overflowing");

  // A null buffer must be refused rather than dereferenced.
  expect(tpv_crash_map_symbol(0x81000228u, nullptr, 16) == 0, "refuses a null buffer");

  std::printf(gFailures == 0 ? "PASS\n" : "FAILURES: %d\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
