// Crash and fatal-path recording. See crash_record.cpp for why this exists and what it costs.
#pragma once

// Installs a SIGABRT handler. Call once, as early as possible in main(), before anything that can
// abort. The abort()/assert() interception itself is link-time (--wrap) and needs no install call.
void tpv_crash_install();

// Names what the calling code was doing, so a later abort() can say "abort() from <this>" instead of
// "abort() from abort()". Optional; use it on fatal paths that would otherwise be anonymous.
void tpv_crash_note(const char* what);
