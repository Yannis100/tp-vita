// Thread census and CPU placement. See cpu_placement.hpp for why each part exists.
#include "cpu_placement.hpp"

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <kubridge.h>

#include <cstdio>
#include <cstring>

namespace tpv::cpu {
namespace {

constexpr int kMaxThreads = 32;

struct Entry {
  int thid = -1;
  const char* name = nullptr;
  Role role = Role::Other;
  Core preferred = Core::User0;
  int original_mask = 0; // what the thread had before this module touched it
  int applied_mask = 0;  // what was accepted, 0 if none
  bool applied = false;
};

Entry sEntries[kMaxThreads];
int sCount = 0;
bool sInitialised = false;
bool sPinned = false;
FourthCore sFourth;
Clocks sClocks;

int mask_of(Core core) noexcept {
  switch (core) {
  case Core::User0:
    return SCE_KERNEL_CPU_MASK_USER_0;
  case Core::User1:
    return SCE_KERNEL_CPU_MASK_USER_1;
  case Core::User2:
    return SCE_KERNEL_CPU_MASK_USER_2;
  case Core::System:
    return SCE_KERNEL_CPU_MASK_SYSTEM;
  }
  return SCE_KERNEL_CPU_MASK_USER_0;
}

Entry* find(int handle) noexcept {
  return handle >= 0 && handle < sCount ? &sEntries[handle] : nullptr;
}

void probe_fourth_core() noexcept {
  sFourth.attempted = true;
  const int thid = sceKernelGetThreadId();
  const int before = sceKernelGetThreadCpuAffinityMask(thid);
  // Ask for a mask that includes the system core. A plain userland build gets an error here; with a
  // plugin that released the core, it is accepted. Either way the port keeps running on the user cores.
  const int want = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_SYSTEM;
  sFourth.set_result = sceKernelChangeThreadCpuAffinityMask(thid, want);
  sFourth.readback = sceKernelGetThreadCpuAffinityMask(thid);
  sFourth.accepted = sFourth.set_result >= 0 && (sFourth.readback & SCE_KERNEL_CPU_MASK_SYSTEM) != 0;
  if (sFourth.accepted) {
    sFourth.verdict = "the 4th core is available (a kernel plugin released it)";
  } else if (sFourth.set_result < 0) {
    sFourth.verdict = "refused: the system core is not released (CapUnlocker absent or inactive)";
  } else {
    sFourth.verdict = "accepted the call but dropped the system bit: treated as unavailable";
  }
  // Restore whatever the thread had, so the probe leaves no trace.
  if (before >= 0) {
    sceKernelChangeThreadCpuAffinityMask(thid, before);
    sFourth.readback = before;
  }
}

} // namespace

const char* role_name(Role role) noexcept {
  switch (role) {
  case Role::Main:
    return "main";
  case Role::Decode:
    return "decode";
  case Role::Audio:
    return "audio";
  case Role::Stream:
    return "stream";
  case Role::Other:
    return "other";
  }
  return "other";
}

void init() noexcept {
  if (sInitialised) {
    return;
  }
  sInitialised = true;
  probe_fourth_core();
  sClocks.arm_mhz = scePowerGetArmClockFrequency();
  // kubridge is linked through its WEAK stub: when the plugin is not loaded the call is still present
  // in the PLT but the module is not, so it returns an error instead of failing to link. That is the
  // only reliable userland signal for "is kubridge actually there", and it is reported, not swallowed.
  const int sys = kuPowerGetSysClockFrequency();
  sClocks.kubridge_ok = sys > 0 ? 1 : 0;
  sClocks.sys_mhz = sys > 0 ? sys : 0;
}

int register_thread_id(int thid, const char* name, Role role, Core preferred) noexcept {
  if (thid < 0 || sCount >= kMaxThreads) {
    return -1;
  }
  Entry& e = sEntries[sCount];
  e.thid = thid;
  e.name = name;
  e.role = role;
  e.preferred = preferred;
  e.original_mask = sceKernelGetThreadCpuAffinityMask(thid);
  return sCount++;
}

int register_thread(const char* name, Role role, Core preferred) noexcept {
  init();
  return register_thread_id(sceKernelGetThreadId(), name, role, preferred);
}

int wanted_mask(int handle) noexcept {
  const Entry* e = find(handle);
  return e != nullptr ? mask_of(e->preferred) : 0;
}

int pin_all() noexcept {
  init();
  int ok = 0;
  for (int i = 0; i < sCount; ++i) {
    Entry& e = sEntries[i];
    const int want = mask_of(e.preferred);
    if (sceKernelChangeThreadCpuAffinityMask(e.thid, want) >= 0) {
      const int back = sceKernelGetThreadCpuAffinityMask(e.thid);
      if ((back & want) != 0) {
        e.applied = true;
        e.applied_mask = back;
        ++ok;
      }
    }
  }
  sPinned = ok > 0;
  return ok;
}

int unpin_all() noexcept {
  int ok = 0;
  for (int i = 0; i < sCount; ++i) {
    Entry& e = sEntries[i];
    // Back to the three user cores: the mask every thread had before, or the SDK's default if the
    // read-back failed. Never leave a thread on the system core.
    const int back = e.original_mask > 0 ? e.original_mask : SCE_KERNEL_CPU_MASK_USER_ALL;
    if (sceKernelChangeThreadCpuAffinityMask(e.thid, back) >= 0) {
      ++ok;
    }
    e.applied = false;
    e.applied_mask = 0;
  }
  sPinned = false;
  return ok;
}

bool pinned() noexcept { return sPinned; }

void set_pinned(bool on) noexcept {
  if (on) {
    pin_all();
  } else {
    unpin_all();
  }
}

const FourthCore& fourth_core() noexcept {
  init();
  return sFourth;
}

Clocks read_clocks() noexcept {
  init();
  sClocks.arm_mhz = scePowerGetArmClockFrequency();
  return sClocks;
}

int try_kubridge_sysclock(int mhz) noexcept {
  init();
  if (!sClocks.kubridge_ok) {
    return 0; // no plugin: report nothing rather than claim a change that did not happen
  }
  if (kuPowerSetSysClockFrequency(mhz) < 0) {
    return 0;
  }
  const int after = kuPowerGetSysClockFrequency();
  sClocks.sys_mhz = after > 0 ? after : 0;
  return sClocks.sys_mhz;
}

void report(LineSink sink) noexcept {
  init();
  sink("cpu: 4th core: %s (set rc=%d readback=0x%05x)", sFourth.verdict, sFourth.set_result,
       static_cast<unsigned>(sFourth.readback));
  if (sClocks.kubridge_ok) {
    sink("cpu: clocks: ARM %d MHz, kubridge sys %d MHz", sClocks.arm_mhz, sClocks.sys_mhz);
  } else {
    sink("cpu: clocks: ARM %d MHz, kubridge UNAVAILABLE (plugin not loaded)", sClocks.arm_mhz);
  }
  sink("cpu: placement is %s; %d thread(s) registered", sPinned ? "PINNED" : "unpinned (scheduler decides)",
       sCount);
  if (sCount == 0) {
    sink("cpu: NOTHING REGISTERED. Every thread is still wherever it was created -- the census cannot see "
         "threads the port did not create (no SceKernelThreadInfo in VitaSDK, no kubridge thread walk).");
    return;
  }
  for (int i = 0; i < sCount; ++i) {
    const Entry& e = sEntries[i];
    const int now = sceKernelGetThreadCpuAffinityMask(e.thid);
    sink("cpu:   %-10s %-6s thid %3d core=%d mask 0x%05x -> 0x%05x%s", e.name != nullptr ? e.name : "?",
         role_name(e.role), e.thid, static_cast<int>(e.preferred), static_cast<unsigned>(e.original_mask),
         static_cast<unsigned>(now), e.applied ? " (applied)" : "");
  }
}

} // namespace tpv::cpu
