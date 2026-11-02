#include "JSystem/JSupport/JSUMemoryStream.h"
#include <cstdio>

int main() {
    u8 buf[64];
    JSUMemoryOutputStream out(buf, sizeof(buf));
    u32 written = 0x01020304u;
    out.write(&written, sizeof(written));
    printf("out pos=%d\n", out.getPosition());
    printf("buf bytes: %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);

    JSUMemoryInputStream in(buf, 4);
    u32 rd = in.readU32();
    printf("readU32 = 0x%08x\n", rd);
    return 0;
}
