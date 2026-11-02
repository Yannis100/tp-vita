/*
 * ppc_math.c - portable stand-ins for the PPC single/double precision math
 * intrinsics that Aurora's ppc_math.h provides. The Tier-1 skeleton doesn't
 * need bit-exact PPC frsqrte matching, so these delegate to libm.
 */
#include <dolphin/ppc_math.h>

#include <math.h>

double __frsqrte(double val) {
    if (val <= 0.0) {
        return val;
    }
    return 1.0 / sqrt(val);
}

double __fres(double val) {
    return 1.0 / val;
}

float __frsqrtes(float val) {
    if (val <= 0.0f) {
        return val;
    }
    return 1.0f / sqrtf(val);
}
