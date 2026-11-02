#ifndef _global_h_
#define _global_h_
/*
 * Vita-side shadow of include/global.h for the Tier-1 game-logic skeleton.
 * Only the macros the portable core references. VERSION defaults to GCN USA.
 */

#define VERSION_GCN_USA          0
#define VERSION_GCN_PAL          1
#define VERSION_GCN_JPN          2
#define VERSION_WII_USA_R0       3

#ifndef VERSION
#define VERSION VERSION_GCN_USA
#endif

#define PLATFORM_GCN    (VERSION >= VERSION_GCN_USA && VERSION <= VERSION_GCN_JPN)
#define PLATFORM_WII    (VERSION >= VERSION_WII_USA_R0)

#ifndef DEBUG
#define DEBUG 0
#endif

#define MSL_INLINE inline

#define ARRAY_SIZE(o) (s32)(sizeof(o) / sizeof(o[0]))
#define ARRAY_SIZEU(o) (sizeof(o) / sizeof(o[0]))

#define ALIGN_PREV(X, N) ((X) & ~((N)-1))
#define ALIGN_NEXT(X, N) ALIGN_PREV(((X) + (N)-1), N)
#define IS_ALIGNED(X, N) (((X) & ((N)-1)) == 0)
#define IS_NOT_ALIGNED(X, N) (((X) & ((N)-1)) != 0)

#define ROUND(n, a) (((u32)(n) + (a)-1) & ~((a)-1))
#define TRUNC(n, a) (((u32)(n)) & ~((a)-1))

#define UNUSED(x) ((void)(x))

#define _SDA_BASE_(dummy) 0
#define _SDA2_BASE_(dummy) 0

#define __REGISTER

#define DUSK_GAME_DATA
#define DUSK_GAME_EXTERN extern DUSK_GAME_DATA

#define DUSK_NOINLINE __attribute__((noinline))

#define FAST_DIV(x, n) (x >> (n / 2))
#define SQUARE(x) ((x) * (x))

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#endif /* _global_h_ */
