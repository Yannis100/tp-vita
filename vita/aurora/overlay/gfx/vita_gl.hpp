// Single include point for OpenGL ES 2 in the Vita backend.
//
// On Vita this is vitaGL (GLES2 + a few vgl* extensions). The same GLES2 code is also built for
// the x86 host test harness (tests/host) against Mesa, which is how the replay, shader generator,
// vertex decoder and texture decoder are checked without a Vita.
#pragma once

#ifdef __vita__
#include <vitaGL.h>
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif
