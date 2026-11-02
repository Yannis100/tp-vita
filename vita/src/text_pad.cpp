// Filler in .rodata (the R+E segment) to keep a gap behind the text segment for vita-elf-create.
//
// The Vita linker script places the data segment at the next 64 KiB boundary after the text segment
// (DATA_SEGMENT_ALIGN(0x10000, 0x10000)), and vita-elf-create appends its SCE metadata (module info,
// stubs, FNIDs; ~5 KiB here) to the end of the text segment. When the text segment happens to end
// less than that below a 64 KiB boundary the build fails with
//   vita-elf-create: Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps
// and any unrelated code change can trigger it. tools/check_text_gap.py reports the gap after every
// link; if it complains, change TPV_TEXT_PAD_KIB (CMake cache variable) by a few tens of KiB.
#ifndef TPV_TEXT_PAD_KIB
#define TPV_TEXT_PAD_KIB 0
#endif

#if TPV_TEXT_PAD_KIB > 0
extern "C" const unsigned char tpv_text_pad[TPV_TEXT_PAD_KIB * 1024] = {1};
#endif
