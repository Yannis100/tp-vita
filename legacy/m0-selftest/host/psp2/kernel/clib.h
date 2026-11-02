/* Host (Linux) shim for psp2/kernel/clib.h so the Tier-1 self-test runs on PC. */
#ifndef PSP2_KERNEL_CLIB_H
#define PSP2_KERNEL_CLIB_H
#include <stdio.h>
#include <stdarg.h>
#define sceClibPrintf printf
#define sceClibVprintf vprintf
#endif
