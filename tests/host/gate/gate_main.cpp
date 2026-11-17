// Gate tests for the switches that are read from the FILESYSTEM, because a Vita has no shell.
//
// Why this exists (2026-11-17). HANDOVER 7.24/7.25 put two features behind `getenv` switches and one
// behind a marker file, and every one of them was verified in Vita3K -- where the harness sets the env
// var. On a real Vita there is no shell, so the env var can never be set, and `decode_worker_allowed()`
// returned false when it was absent. The decode worker was therefore UNREACHABLE on hardware, while the
// docs said it was "behind a flag". The console A/B this feature exists to settle could not be run.
//
// That is a class of defect this port keeps re-finding: a switch whose failure mode is silence. It looks
// wired up, it logs nothing, and the only symptom is a feature that never runs. A compile cannot catch
// it and the emulator masks it. So the decision logic is exercised here, directly, in all four
// combinations and in the spellings that a human actually types.
//
// The logic below MUST stay byte-for-byte equivalent to `decode_worker_allowed()` in
// vita/aurora/overlay/gfx/vita_gl_replay.cpp. It is duplicated deliberately: that function is inside an
// anonymous namespace in a translation unit that needs vitaGL/GL to link, so it cannot be called from a
// host test without dragging in the whole renderer. The duplication is the cost of testing it at all,
// and this comment is the warning that they are two copies.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_fail = 0;
int g_checks = 0;

void expect(bool got, bool want, const char* what) {
  ++g_checks;
  const bool ok = got == want;
  if (!ok) {
    ++g_fail;
  }
  std::printf("%-58s got=%d want=%d  %s\n", what, got ? 1 : 0, want ? 1 : 0, ok ? "ok" : "FAIL");
}

bool exists(const std::string& p) {
  if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

void touch(const std::string& p) {
  if (std::FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fclose(f);
  }
}

void remove_file(const std::string& p) { std::remove(p.c_str()); }

// A faithful copy of the shipped decision. Order: disable-file, disable-env, enable-env, enable-file.
bool allowed(const std::string& dir) {
  const std::string noPath = dir + "/no_decode_worker";
  const std::string yesPath = dir + "/decode_worker";
  const auto says_off = [](const char* flag) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    char buf[8] = {};
    size_t n = 0;
    for (; flag[n] != '\0' && n < sizeof(buf) - 1; ++n) {
      buf[n] = lower(flag[n]);
    }
    return n == 0 || std::strcmp(buf, "0") == 0 || std::strcmp(buf, "no") == 0 ||
           std::strcmp(buf, "off") == 0 || std::strcmp(buf, "false") == 0 ||
           std::strcmp(buf, "disable") == 0 || std::strcmp(buf, "disabled") == 0;
  };
  const char* flag = std::getenv("TPV_ENABLE_DECODE_WORKER");
  if (exists(noPath)) {
    return false;
  }
  if (flag != nullptr && says_off(flag)) {
    return false;
  }
  if (flag != nullptr) {
    return true;
  }
  if (exists(yesPath)) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  const char* tmp = std::getenv("TMPDIR");
  const std::string dir = std::string(tmp != nullptr ? tmp : "/tmp") + "/tpv_gate";
  const std::string yes = dir + "/decode_worker";
  const std::string no = dir + "/no_decode_worker";
#ifdef _WIN32
  const int made = std::system(("mkdir \"" + dir + "\" 2>NUL").c_str());
#else
  const int made = std::system(("mkdir -p \"" + dir + "\"").c_str());
#endif
  (void)made; // the cases below fail loudly if the directory is missing, so the code is enough
  remove_file(yes);
  remove_file(no);
  unsetenv("TPV_ENABLE_DECODE_WORKER");

  std::printf("-- the defect this test exists for --\n");
  // This is what a Vita does today: no shell, so no env var. It must be OFF by default, but it must be
  // POSSIBLE to turn on. The next case is the fix; the one after it would have been the bug.
  expect(allowed(dir), false, "bare console, no files, no env -> OFF");
  touch(yes);
  expect(allowed(dir), true, "decode_worker file present -> ON  (the fix for the console)");

  std::printf("\n-- precedence: the control arm must always be forceable OFF --\n");
  touch(no);
  expect(allowed(dir), false, "no_decode_worker overrides decode_worker -> OFF");
  setenv("TPV_ENABLE_DECODE_WORKER", "1", 1);
  expect(allowed(dir), false, "no_decode_worker overrides env=1 -> OFF");
  remove_file(no);
  expect(allowed(dir), true, "env=1 with no disable-file -> ON");

  std::printf("\n-- spellings of OFF, the 7.25 trap --\n");
  // The first version tested a leading character (0/n/f), so "off" started with 'o' and ENABLED the
  // worker: an A/B control arm that ran the feature under test. This test caught that.
  for (const char* v : {"0", "no", "off", "false", "OFF", "False", "disable", ""}) {
    setenv("TPV_ENABLE_DECODE_WORKER", v, 1);
    char what[96];
    std::snprintf(what, sizeof(what), "env=\"%s\" -> OFF", v);
    expect(allowed(dir), false, what);
  }
  // And a truthy spelling must still be ON, or the test above could pass by always returning false.
  for (const char* v : {"1", "yes", "on", "true", "ON"}) {
    setenv("TPV_ENABLE_DECODE_WORKER", v, 1);
    char what[96];
    std::snprintf(what, sizeof(what), "env=\"%s\" -> ON", v);
    expect(allowed(dir), true, what);
  }

  remove_file(yes);
  unsetenv("TPV_ENABLE_DECODE_WORKER");
  expect(allowed(dir), false, "cleaned up (no file, no env) -> OFF");

  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail != 0 ? 1 : 0;
}
