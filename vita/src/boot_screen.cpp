// Display side of the pre-game error screens. The recipe (a CDRAM block as framebuffer, 256 KiB aligned,
// sceDisplaySetFrameBuf in A8B8G8R8) is the one VitaSDK's debug-screen samples use. It builds, but it has
// not been run on a Vita or in Vita3K.
#include "boot_screen.hpp"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace tpv {
namespace {
constexpr int kWidth = 960;
constexpr int kHeight = 544;
constexpr int kScale = 2;
constexpr int kMargin = 16;
constexpr unsigned kCdramAlign = 256 * 1024;
constexpr unsigned kFaceButtons = SCE_CTRL_CIRCLE | SCE_CTRL_CROSS;
constexpr unsigned kFramePeriodUs = 16 * 1000;

unsigned buttons_now() {
    SceCtrlData pad{};
    return sceCtrlPeekBufferPositive(0, &pad, 1) > 0 ? pad.buttons : 0;
}
} // namespace

bool show_screen_and_wait(const Screen& screen) {
    const unsigned bytes = (kWidth * kHeight * 4 + kCdramAlign - 1) & ~(kCdramAlign - 1);
    const SceUID block = sceKernelAllocMemBlock("tpv_bootscreen", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
    if (block < 0) {
        return false;
    }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(block, &base) < 0 || base == nullptr) {
        sceKernelFreeMemBlock(block);
        return false;
    }

    // Render in ordinary memory and copy once: the CDRAM mapping is slow for scattered single-pixel writes.
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    Canvas canvas{pixels.data(), kWidth, kHeight, kWidth};
    render(canvas, layout(screen, text_cols(kWidth, kScale, kMargin), text_rows(kHeight, kScale, kMargin)), kScale,
           kMargin);
    std::memcpy(base, pixels.data(), pixels.size() * sizeof(uint32_t));

    SceDisplayFrameBuf fb{};
    fb.size = sizeof(fb);
    fb.base = base;
    fb.pitch = kWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = kWidth;
    fb.height = kHeight;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    // A face button still held from launching the app must not dismiss the screen at once: wait for release,
    // then for a fresh press.
    while ((buttons_now() & kFaceButtons) != 0) {
        sceKernelDelayThread(kFramePeriodUs);
    }
    while ((buttons_now() & kFaceButtons) == 0) {
        sceKernelDelayThread(kFramePeriodUs);
    }
    return true;
}

} // namespace tpv
