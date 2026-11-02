/*
 * vita_runtime.c - startup settings for the TP-Tier1 Vita skeleton.
 * Adapted from the vita-port skill's assets/vita_runtime.c.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#include <stdio.h>
#include <string.h>

/* malloc() is backed by ONE memblock of this size taken at startup. */
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;

/* Sony libc heap (Sony libraries, not newlib malloc). */
unsigned int sceLibcHeapSize = 4 * 1024 * 1024;

/* Main-thread stack: deep decomp recursion crashes silently with the default. */
const unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

/* Lower = higher priority. */
const int sceUserMainThreadPriority = 0x60;

#define VITA_DATA_DIR "ux0:data/tpvitaskel"

static void ensure_data_dir(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VITA_DATA_DIR, 0777);
}

static void set_clocks(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

void vita_runtime_init(void) {
    set_clocks();
    ensure_data_dir();
    sceClibPrintf("tp-vita skeleton: heap=192MiB stack=8MiB clocks 444/222/222/166\n");
}
