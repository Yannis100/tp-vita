#ifndef DOLPHIN_TYPES_H
#define DOLPHIN_TYPES_H
/*
 * Vita-side declaration layer for TP decomp game logic (Tier-1 skeleton).
 * Mirrors the declarations the portable JSystem/JMath/JSupport units need from
 * upstream extern/aurora/include/dolphin/types.h — WITHOUT any Aurora internals.
 */

#include <stdint.h>
#include <stddef.h>

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile u8  vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8  vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float  f32;
typedef double f64;
typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef char *Ptr;

typedef int BOOL;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#if defined(__cplusplus) && __cplusplus < 201103L
#define nullptr NULL
#endif

#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#define AT_ADDRESS(addr)

#ifdef __MWERKS__
#define __REGISTER register
#else
#define __REGISTER
#endif

#ifndef DECL_WEAK
#define DECL_WEAK __attribute__((weak))
#endif

#endif /* DOLPHIN_TYPES_H */
