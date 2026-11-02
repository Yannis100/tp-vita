/*
 * os.cpp - minimal Vitaport of the Dolphin OS/stub surface used by the
 * Tier-1 TP game-logic skeleton.
 *
 * OSReport/OSPanic print to the Vita debug/stdout (sceClibPrintf) so that on a
 * running Vita (or Vita3K) the skeleton reports progress/errors. Everything
 * here is weak so a real port can override it.
 */
#include <dolphin/os.h>
#include <psp2/kernel/clib.h>

#include <stdarg.h>
#include <stdio.h>

extern "C" {

u8 __OSReport_disable;
u8 __OSReport_Error_disable;
u8 __OSReport_Warning_disable;
u8 __OSReport_System_disable;
u8 __OSReport_enable;

void OSReportInit(void) {
    __OSReport_disable = 0;
    __OSReport_Error_disable = 0;
    __OSReport_Warning_disable = 0;
    __OSReport_System_disable = 0;
    __OSReport_enable = 1;
}

__attribute__((weak)) void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    sceClibVprintf(msg, args);
    va_end(args);
}

__attribute__((weak)) void OSReport_Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sceClibVprintf(fmt, args);
    va_end(args);
}

__attribute__((weak)) void OSReport_Warning(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sceClibVprintf(fmt, args);
    va_end(args);
}

__attribute__((weak)) void OSReport_System(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sceClibVprintf(fmt, args);
    va_end(args);
}

__attribute__((weak)) void OSReport_FatalError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sceClibVprintf(fmt, args);
    va_end(args);
}

__attribute__((weak)) void OSReportDisable(void) {
    __OSReport_disable = 1;
}

__attribute__((weak)) void OSReportEnable(void) {
    __OSReport_disable = 0;
}

__attribute__((weak)) void OSReportForceEnableOff(void) {}

__attribute__((weak)) void OSReportForceEnableOn(void) {}

__attribute__((weak)) void OSAttention(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    sceClibVprintf(msg, args);
    va_end(args);
}

__attribute__((weak, noreturn)) void OSPanic(const char* file, int line, const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    sceClibPrintf("[TP-VITA PANIC] %s:%d: ", file, line);
    sceClibVprintf(msg, args);
    va_end(args);
    sceClibPrintf("\n");
    for (;;) {
    }
}

}  // extern "C"
