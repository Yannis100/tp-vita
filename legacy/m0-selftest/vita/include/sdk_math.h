#ifndef VITA_SDK_MATH_H
#define VITA_SDK_MATH_H
/*
 * Vita-side stand-in for libs/revolution/include/revolution/sdk_math.h.
 * The upstream MSL fake-math is PPC-specific; on ARM we use libm directly.
 */

#include <math.h>
#include <stdint.h>

#if defined(__GNUC__)
#define __frsqrte __builtin_sqrt
#define __fres(x) (1.0f / (x))
#endif

#endif /* VITA_SDK_MATH_H */
