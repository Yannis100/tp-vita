// Vita: borealis' crash reporter uses POSIX ucontext/sigaction. On Vita, crashes are captured by the
// system core-dump handler (decode with vita-parse-core), so there is nothing to install.
#include <borealis/crash.hpp>

namespace borealis::crash {
void install() {}
}  // namespace borealis::crash
