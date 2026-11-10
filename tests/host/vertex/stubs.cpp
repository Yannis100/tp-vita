// Link-time stubs for the vertex decoder suite.
//
// Why this file exists: vita_vertex.cpp reports through aurora::Module, which is a thin wrapper over
// aurora::log_internal() (see logging.hpp). log_internal() reads aurora::g_config.logLevel, so linking
// the decoder against logging.cpp is not enough -- g_config must exist too. The suite is a plain
// computation test: it decodes vertex buffers and compares them, and it never initialises Aurora, so
// a default-constructed config is correct rather than a shortcut. e2e does the same thing in
// e2e_stubs.cpp.
//
// Without this the suite fails to link with `undefined reference to aurora::log_internal` (and, once
// that one is fixed, `undefined reference to aurora::g_config`), which is how it was found on
// 2026-11-05: the suite had stopped being runnable and nothing said so because no test asserted
// "the suite builds".

#include <aurora/aurora.h>

namespace aurora {
AuroraConfig g_config{};
}
