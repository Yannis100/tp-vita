#ifndef DOLPHIN_PPC_MATH_H
#define DOLPHIN_PPC_MATH_H
/*
 * Vita-side declaration layer for TP decomp game logic (Tier-1 skeleton).
 * Provides the PPC math surfaces referenced by JMath.h / helpers/math.h.
 * Upstream also declares __frsqrtes inline; that one is INLINE in JMath.h and
 * must not be re-declared here to avoid a redeclaration conflict.
 */

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

double __frsqrte(double val);
double __fres(double val);

#ifdef __cplusplus
}
#endif

/* JMAFastSqrt() calls frsqrte() (no underscores) in the non-MWERKS path. */
static inline double frsqrte(double val) {
    if (val <= 0.0) {
        return val;
    }
    return 1.0 / sqrt(val);
}

#endif /* DOLPHIN_PPC_MATH_H */
