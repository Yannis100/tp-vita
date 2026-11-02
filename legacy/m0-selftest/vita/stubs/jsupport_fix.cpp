/*
 * jsupport_fix.cpp - Vita-side out-of-line definitions for JSUMemoryOutputStream
 * virtuals that upstream declares but never defines (they were expected to
 * resolve to the base JSURandomOutputStream implementations, but the vtable is
 * emitted by JSUMemoryStream.cpp which references them directly).
 *
 * See libs/JSystem/include/JSystem/JSupport/JSUMemoryStream.h.
 */
#include "JSystem/JSupport/JSUMemoryStream.h"

s32 JSUMemoryOutputStream::seek(s32 pos, JSUStreamSeekFrom seekFrom) {
    return seekPos(pos, seekFrom);
}

s32 JSUMemoryOutputStream::getAvailable() const {
    return getLength() - getPosition();
}
