// Thread census and CPU placement (HANDOVER 7.23/7.24).
//
// Why this exists: nothing in this port set a CPU affinity mask, so the game thread, the GL replay,
// dusklight's detached memcard/file threads and audio were all placed by the scheduler's default. Three
// user cores were available and used by accident. This module makes placement explicit and observable.
//
// Three separate things, deliberately kept apart because they have different costs:
//
//   1. CENSUS. Every thread the port creates registers itself (name, default core, priority) and its
//      mask can be read back. A report lists each one with the core it is actually running on. The
//      VitaSDK does NOT define SceKernelThreadInfo and provides no thread-enumeration API, and kubridge
//      exposes no affinity call either -- so a census of threads the port did not create is not
//      possible from userland. This is a REGISTRY, not a walk of the kernel's thread list; the report
//      says so rather than implying it saw everything.
//
//   2. AFFINITY. sceKernelChangeThreadCpuAffinityMask on registered threads. Pinning is off by default
//      and toggled at runtime, because the layout is a hypothesis that only hardware can settle: on a
//      Cortex-A9 with a GPU driver thread, concentrating work can hurt as easily as help.
//
//   3. THE 4TH CORE. SCE_KERNEL_CPU_MASK_SYSTEM (0x80000) is normally refused for a user thread. If a
//      kernel plugin has released it (CapUnlocker), including it in a mask is accepted. So the probe is:
//      TRY it, read the mask back, and report what the console actually allowed -- never assume.
//
// Everything degrades to a no-op. A missing plugin, a rejected mask, a refused kubridge call: the port
// runs exactly as it did before, and the report says which parts were unavailable and why.
#pragma once

#include <cstdint>

namespace tpv::cpu {

// The core a thread should prefer. USER_0/1/2 are the three cores a game may use; SYSTEM is the 4th,
// which requires a kernel plugin to have released it.
enum class Core : uint8_t { User0 = 0, User1 = 1, User2 = 2, System = 3 };

// Threads the port knows about. Adding one here is how a thread becomes visible to the census.
enum class Role : uint8_t {
  Main,       // the game thread: DuskMain, the FIFO/command processor, the GL replay
  Decode,     // the optional decode worker (vita_gl_replay.cpp, TPV_ENABLE_DECODE_WORKER)
  Audio,      // SDL/audio mixer
  Stream,     // file and memory-card threads (dusklight's detached std::threads)
  Other,
};

const char* role_name(Role role) noexcept;

// Monotonic-free: call once, before any thread that wants to be counted. Safe to call twice.
void init() noexcept;

// Register the CALLING thread. Returns a handle for pinning, or -1 if the registry is full.
// `preferred` is the core the layout suggests; nothing is changed until apply().
int register_thread(const char* name, Role role, Core preferred) noexcept;

// Register a thread by id (for a worker the port created itself and can name).
int register_thread_id(int thid, const char* name, Role role, Core preferred) noexcept;

// The mask the layout currently wants for this thread, or 0 if unknown.
int wanted_mask(int handle) noexcept;

// Pin / unpin one thread or all of them. pin_all() returns how many masks were accepted by the kernel.
// Unpinning restores SCE_KERNEL_CPU_MASK_USER_ALL (the three user cores).
int pin_all() noexcept;
int unpin_all() noexcept;
bool pinned() noexcept;

// Toggle used for A/B on hardware. Reads and writes sPinned.
void set_pinned(bool on) noexcept;

// The 4th-core question, answered by trying rather than by assuming.
//   `requested` -- SCE_KERNEL_CPU_MASK_SYSTEM was included in a mask and the kernel returned >= 0
//   `accepted`  -- the mask read back still contains it
// A plugin that is installed but for a different firmware, or absent, fails one of those two.
struct FourthCore {
  bool attempted = false;
  int set_result = 0;   // 0 = accepted
  int readback = 0;     // mask the kernel reports afterwards
  bool accepted = false;
  const char* verdict = "not probed";
};
const FourthCore& fourth_core() noexcept;

// Reads the CPU clock as the kernel reports it, and whether kubridge's sysclock call is available.
// kubridge's kuPowerSetSysClockFrequency only means something on a plugin build; a weak stub returns
// an error when the plugin is missing, which is exactly the signal used here.
struct Clocks {
  int arm_mhz = 0;          // scePowerGetArmClockFrequency
  int kubridge_ok = 0;      // 0 = unavailable, 1 = kuPowerGetSysClockFrequency answered
  int sys_mhz = 0;          // kuPowerGetSysClockFrequency, if it answered
};
Clocks read_clocks() noexcept;

// Raise the system clock through kubridge if it is available. Returns the frequency reported after the
// attempt, or 0. Deliberately not called by default: it is a hardware-tuning step, not a port default.
int try_kubridge_sysclock(int mhz) noexcept;

// Writes the census + mask + 4th-core + clock state as log lines. `log_line` must be a printf-style
// sink the caller owns (boot.log or the game log), so this file needs no logging dependency.
using LineSink = void (*)(const char* fmt, ...);
void report(LineSink sink) noexcept;

} // namespace tpv::cpu
