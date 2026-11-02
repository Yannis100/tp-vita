// Crash and fatal-path recorder: makes a silent death say what it was.
//
// Why this exists. Two Vita3K runs died with
//   Undefined instruction at 0x8227A58E ... Trace/breakpoint trap (core dumped)
// and that address resolves to _kill_r, so the guest raised a signal against itself: it was our
// process calling abort(). Nothing recorded which abort. Every abort site reachable from game code
// is silent -- PPCHalt() and OSSwitchStack()/OSSwitchFiber() are bare `abort()`, and
// ConfigVarBase::markRegistered()/unmarkRegistered() abort on a double register with no message.
// The game log held no critical line at all, and boot.log came out empty, so the artefacts said
// only "it died". That is the defect this file fixes; it is not about the crash being rare.
//
// Two mechanisms, both writing to ux0:data/tpvita/crash.log, one flushed line at a time, with the
// previous run kept as crash.log.1:
//
//   1. __wrap_abort() catches every abort() in the process. newlib defines abort as a strong global,
//      so `-Wl,--wrap=abort` redirects every call site without touching dusklight, which keeps the
//      "behaviour comes from the decompilation" posture intact. It records which function aborted,
//      the link-map symbol nearest the return address (so the call site is named), and whether the
//      abort came from an assert (__assert_func is intercepted too).
//
//   2. A SIGABRT handler that explains a HOST-side abort. A failed std::thread constructor throws
//      std::system_error, and if that is uncaught the runtime calls std::terminate -> abort from
//      inside libstdc++, where __wrap_abort never sees it. The handler dumps /proc/self/maps
//      because that is the only way to learn the base address the ELF was actually loaded at.
//
// The maps dump matters more than it looks. Every earlier symbolisation here had to assume the
// link-time address equals the run-time address, which is why naming 0x8227A58E needed grepping
// section headers and then a 666 MB debug ELF. With the base recorded, `addr2line -e tpv_vita
// $((0x8227a58e))` is only valid if the base happens to be the link address; recording it makes
// the arithmetic explicit instead of assumed.
//
// Cost when nothing goes wrong: __wrap_abort is never called, and the SIGABRT handler is one
// sigaction at startup. No per-frame work.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <signal.h>
#include <unistd.h>

// Naming the aborting function requires either an unwind or a symbol map. The Vita has no
// backtrace(); the link map (see TPV_CRASH_MAP in CMakeLists.txt) is the portable answer.
extern "C" int tpv_crash_map_symbol(uintptr_t address, char* out, unsigned outSize);

// Provided by `-Wl,--wrap=abort`: the linker renames every reference to abort() to __wrap_abort and
// every definition of it to __real_abort, so the real newlib abort stays reachable here.
extern "C" void __real_abort(void) __attribute__((noreturn));

namespace {

constexpr const char* kDir = "ux0:data/tpvita";
constexpr const char* kLogPath = "ux0:data/tpvita/crash.log";
constexpr const char* kPrevPath = "ux0:data/tpvita/crash.log.1";

FILE* gFile = nullptr;
bool gOpened = false;

// Everything here runs on a fatal path, so it must not allocate, must not use std::string, and must
// tolerate being re-entered (an abort inside abort is how this class of bug usually looks).
void rotate_and_open() {
  if (gOpened) {
    return;
  }
  gOpened = true;
  std::remove(kPrevPath);
  std::rename(kLogPath, kPrevPath);
  gFile = std::fopen(kLogPath, "wb");
}

void emit(const char* text) {
  rotate_and_open();
  if (gFile != nullptr) {
    std::fputs(text, gFile);
    std::fputc('\n', gFile);
    std::fflush(gFile); // the whole point: survive the death that follows
  }
  // Also to stderr. In Vita3K this lands in vita3k.log; on hardware it reaches a debug console.
  std::fputs("[tpv-crash] ", stderr);
  std::fputs(text, stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void emit_hex(const char* label, uintptr_t value) {
  char line[160];
  std::snprintf(line, sizeof(line), "%s=0x%08lx", label, static_cast<unsigned long>(value));
  emit(line);
}

void emit_symbol(const char* label, uintptr_t address) {
  char name[192];
  name[0] = '\0';
  if (tpv_crash_map_symbol(address, name, sizeof(name)) != 0) {
    char line[320];
    std::snprintf(line, sizeof(line), "%s=0x%08lx  %s", label, static_cast<unsigned long>(address), name);
    emit(line);
  } else {
    char line[160];
    std::snprintf(line, sizeof(line), "%s=0x%08lx  (not in the link map)", label,
                  static_cast<unsigned long>(address));
    emit(line);
  }
}

char gWhat[96] = "unknown";
bool gInHandler = false;

// /proc/self/maps is the only trustworthy statement of where the ELF was loaded. A truncated
// version is fine: the first mapping line carries the base and the path.
void emit_maps() {
  FILE* maps = std::fopen("/proc/self/maps", "rb");
  if (maps == nullptr) {
    emit("maps: /proc/self/maps unavailable");
    return;
  }
  char line[256];
  int count = 0;
  while (count < 6 && std::fgets(line, sizeof(line), maps) != nullptr) {
    const size_t n = std::strlen(line);
    if (n > 0 && line[n - 1] == '\n') {
      line[n - 1] = '\0';
    }
    char tagged[320];
    std::snprintf(tagged, sizeof(tagged), "maps: %s", line);
    emit(tagged);
    ++count;
  }
  std::fclose(maps);
}

// VitaSDK's newlib signal.h has no SA_SIGINFO, no sa_sigaction and no siginfo_t::si_addr, so this is
// the plain one-argument form. Nothing is lost for SIGABRT: si_addr is meaningless there, and what
// matters is the recorded context plus the load base from /proc/self/maps.
void on_sigabrt(int) {
  if (gInHandler) {
    // Re-entered: do the least possible and get out, or the process hangs instead of dying.
    _exit(134);
  }
  gInHandler = true;
  emit("=== SIGABRT ===");
  char line[192];
  std::snprintf(line, sizeof(line), "last_context=%s", gWhat);
  emit(line);
  emit_maps();
  emit("If this came from libstdc++ rather than game code, suspect an uncaught exception: a failed");
  emit("std::thread constructor throws std::system_error, which terminates.");
  if (gFile != nullptr) {
    std::fclose(gFile);
    gFile = nullptr;
  }
  // Restore the default action and re-raise so the exit status is unchanged.
  ::signal(SIGABRT, SIG_DFL);
  ::raise(SIGABRT);
}

} // namespace

extern "C" {

// ---- the linker's abort() ------------------------------------------------------------------
// `-Wl,--wrap=abort` makes every undefined reference to abort resolve here, while newlib's real
// abort() is still reachable as __real_abort(). That is why dusklight needs no edits.
void __wrap_abort(void) {
  const char* from = "abort()";
  if (std::strcmp(gWhat, "unknown") != 0) {
    from = gWhat;
  }
  char line[192];
  std::snprintf(line, sizeof(line), "=== abort() from %s ===", from);
  emit(line);

  // __builtin_return_address(0) is this wrapper's caller, because __wrap_abort is a real function.
  // Anything above 0 is unsafe without frame pointers, so only level 0 is used.
  const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  emit_symbol("abort_caller", caller);

  __real_abort();
}

// ---- assert() ------------------------------------------------------------------------------
// newlib routes a failed assert here. Recording the expression, file and line is the single most
// useful thing this file does, because an assert message is otherwise lost before the log flushes.
void __wrap___assert_func(const char* file, int line, const char* func, const char* expr) {
  std::snprintf(gWhat, sizeof(gWhat), "assert(%s) in %s", expr != nullptr ? expr : "?",
                func != nullptr ? func : "?");
  char text[512];
  std::snprintf(text, sizeof(text), "=== assert(e=%s) at %s:%d in %s ===", expr != nullptr ? expr : "?",
                file != nullptr ? file : "?", line, func != nullptr ? func : "?");
  emit(text);
  __wrap_abort();
}

} // extern "C"

void tpv_crash_install() {
  // Touch nothing unless there is a reason to; opening the file is deferred to the first line, so a
  // clean run leaves no empty crash.log behind. Plain signal() rather than sigaction(): VitaSDK's
  // newlib exposes neither SA_SIGINFO nor sa_sigaction, and they carry nothing useful for SIGABRT.
  ::signal(SIGABRT, on_sigabrt);
}

// The game's fatal paths can call this to name what they were doing before they abort. Kept
// separate from abort() so a caller can set a context string and let the wrapper do the reporting.
void tpv_crash_note(const char* what) {
  if (what != nullptr) {
    std::snprintf(gWhat, sizeof(gWhat), "%s", what);
  }
}
