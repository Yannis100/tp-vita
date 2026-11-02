#ifndef JSYSTEM_H
#define JSYSTEM_H
/*
 * Vita-side lean shadow of libs/JSystem/include/JSystem/JSystem.h.
 * Upstream includes JSystem.pch (J3D/JAudio headers) and relies on the full
 * game PCH for __REGISTER / DUSK_GAME_DATA. The Tier-1 portable core only
 * needs these macros plus the OS report helpers.
 */

#include "global.h"

#ifndef __REGISTER
#define __REGISTER
#endif

#include "os_report.h"

#endif /* JSYSTEM_H */
