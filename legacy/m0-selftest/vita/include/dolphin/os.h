#ifndef DOLPHIN_OS_H
#define DOLPHIN_OS_H
/*
 * Vita-side declaration layer for TP decomp game logic (Tier-1 skeleton).
 * Minimal OS API surface used by the portable core. Implemented in
 * vita/stubs/os.cpp.
 */

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void OSReportInit(void);

DECL_WEAK void OSReport(const char* msg, ...);
DECL_WEAK void OSReport_Error(const char* fmt, ...);
DECL_WEAK void OSReport_Warning(const char* fmt, ...);
DECL_WEAK void OSReport_System(const char* fmt, ...);
DECL_WEAK void OSReport_FatalError(const char* fmt, ...);
DECL_WEAK void OSReportDisable(void);
DECL_WEAK void OSReportEnable(void);
DECL_WEAK void OSReportForceEnableOff(void);
DECL_WEAK void OSReportForceEnableOn(void);

DECL_WEAK void OSPanic(const char* file, int line, const char* msg, ...);

#ifndef NORETURN
#define NORETURN
#endif

#define OSErrorLine(line, ...) OSPanic(__FILE__, line, __VA_ARGS__)

#define ASSERTLINE(line, cond, ...) ((cond) || (OSPanic(__FILE__, line, __VA_ARGS__), 0))
#define ASSERTMSGLINE(line, cond, msg) ((cond) || (OSPanic(__FILE__, line, msg), 0))
#define ASSERTMSGLINEV(line, cond, ...) ((cond) || (OSPanic(__FILE__, line, __VA_ARGS__), 0))
#define ASSERT(cond) ASSERTLINE(__LINE__, cond)
#define ASSERTMSG(cond, msg) ASSERTMSGLINEV(__LINE__, cond, msg)

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG
#define DOLPHIN_ASSERT(...) ASSERT(__VA_ARGS__)
#else
#define DOLPHIN_ASSERT(...) (void)0
#endif

#ifdef __cplusplus
}
#endif

#endif /* DOLPHIN_OS_H */
