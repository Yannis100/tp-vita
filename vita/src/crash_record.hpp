// Crash and fatal-path recording. See crash_record.cpp for why this exists and what it costs.
#pragma once

#include <cstdint>

// Installs a SIGABRT handler. Call once, as early as possible in main(), before anything that can
// abort. The abort()/assert() interception itself is link-time (--wrap) and needs no install call.
void tpv_crash_install();

// Names what the calling code was doing, so a later abort() can say "abort() from <this>" instead of
// "abort() from abort()". Optional; use it on fatal paths that would otherwise be anonymous.
void tpv_crash_note(const char* what);

// Resolves `address` to a symbol name from the symbol table shipped in the VPK. Returns 1 on a
// match, 0 otherwise (no table, no match, or an address past the end of its symbol).
extern "C" int tpv_crash_map_symbol(uintptr_t address, char* out, unsigned outSize);

// 1 if a name from the symbol table is safe to trust for this run, 0 if it is not (no table, or a
// table from a different build).
//
// The table is a separate file, so it can be updated without a rebuild -- which also means it can
// be wrong. It records the link-time address of tpv_crash_table_anchor; validity is that address
// matching the live one. Everything that reports a name must ask this first, because a name from a
// mismatched table is confidently wrong. One such name already cost a session here: an address was
// reported as borealis::log::file_descriptor() when the real site was write_fatal() in log.cpp.
extern "C" int tpv_crash_table_is_valid();

// The two addresses the validity test compares: where the anchor sits according to the table, and
// where it actually sits now. Reporting both makes a stale table diagnosable ("off by 0x1c, so this
// table is from a different link") instead of merely distrusted. Returns 0 if no table exists or it
// has no anchor row, in which case the outputs are zero.
extern "C" int tpv_crash_table_anchor_pair(uintptr_t* tableAddress, uintptr_t* liveAddress);

// Test seam: point the resolver at a table built elsewhere. Not called by the game.
extern "C" void tpv_crash_table_set_path(const char* path);
