/**
 * Vita port: placeholder stubs for the remaining GameCube/Wii system APIs that
 * are not provided by Aurora's target-neutral lib (tpv_aurora covers OS, VI,
 * SI, PAD, DVD, GD, card, mtx). These cover AI (audio interface), AR (audio
 * RAM), ARQ, DSP and a few remaining OS/VI entry points the game references.
 *
 * All are no-ops / sane defaults for now. Audio on Vita will be implemented in
 * the real backend stage (SceAudio / vitaGL are candidates).
 */

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <dolphin/dsp.h>
#include <dolphin/os.h>
#include <dolphin/types.h>
#include <dolphin/vi.h>

#include <cstring>

extern "C" {

// ---------------------------------------------------------------------------
// dolphip/ai.h
// ---------------------------------------------------------------------------
BOOLEAN AIGetDSPSampleRate(void) { return 0; }
void AISetDSPSampleRate(BOOLEAN rate) { (void)rate; }
void AIInit(void) {}
void AIReset(void) {}

// ---------------------------------------------------------------------------
// dolphip/ar.h
// ---------------------------------------------------------------------------
u32 ARGetSize(void) { return 0; }
void ARInit(u32* stackIndexArray, u32 numChannels) { (void)stackIndexArray; (void)numChannels; }
void ARReset(void) {}
u32 ARAlloc(u32 length) { (void)length; return 0; }
u32 ARFree(u32 address) { (void)address; return 0; }
u32 ARGetBaseAddress(void) { return 0; }
u32 ARGetBaseAddressToBottom(void) { return 0; }
void ARStartDMA(u32 dir, u32 chan, void* dramAddr, u32 aramAddr, u32 length) {
    (void)dir; (void)chan; (void)dramAddr; (void)aramAddr; (void)length;
}
u32 ARGetDMAStatus(void) { return 0; }
void ARSetSize(u32 size) { (void)size; }

// ---------------------------------------------------------------------------
// dolphip/arq.h
// ---------------------------------------------------------------------------
void ARQInit(void) {}
void ARQReset(void) {}
void ARQPostRequest(ARQRequest* req, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
    ARQCallback callback) {
    (void)req; (void)owner; (void)type; (void)priority; (void)source; (void)dest; (void)length; (void)callback;
}
void ARQSetPriority(u32 priority) { (void)priority; }
u32 ARQGetPriority(void) { return 0; }

// ---------------------------------------------------------------------------
// dolphip/dsp.h
// ---------------------------------------------------------------------------
void DSPInit(void) {}
void DSPReset(void) {}
void DSPHalt(void) {}
void DSPUnhalt(void) {}
void DSPCheckMailToDSP(void) {}
void DSPCheckMailFromDSP(void) {}
void DSPSendMailToDSP(u32 msg) { (void)msg; }
void DSPRequestMailFromDSP(u32* msg) { if (msg != nullptr) *msg = 0; }
void DSPAssertInt(void) {}
u32 DSPCheckMailFromDSPInternal(void) { return 0; }
u32 DSPCheckMailToDSPInternal(void) { return 0; }
void DSPSendMailToDSPInternal(u32 msg) { (void)msg; }
void DSPRequestMailFromDSPInternal(u32* msg) { if (msg != nullptr) *msg = 0; }

// ---------------------------------------------------------------------------
// dolphip/vi.h (extras not in tpv_aurora)
// ---------------------------------------------------------------------------
void VIEnableDimming(void) {}
void VIDisableDimming(void) {}
u32 VIGetNextFrameBuffer(void) { return 0; }
void VIWaitForRetrace(void) {}
void VIReset(void) {}
void VIConfigure(void) {}
void VIConfigurePan(u16 x0, u16 y0, u16 w, u16 h) { (void)x0; (void)y0; (void)w; (void)h; }
void VISetBlack(u8 black) { (void)black; }
void VISetNextFrameBuffer(void* fb) { (void)fb; }
void VIInit(void) {}

}  // extern "C"
