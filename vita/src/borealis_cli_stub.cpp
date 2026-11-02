/**
 * Vita port: stub for borealis CLI options parsing.
 *
 * game_main (m_Do_main.cpp) parses `cxxopts` options and calls
 * borealis::cli::StandardOptions / borealis::cli::parse / add_standard_options.
 * The real borealis cli module is tied to desktop CLI conventions. On Vita we
 * simply provide empty defaults so argument parsing succeeds with no arguments.
 */

#include <borealis/cli.hpp>

#include <cxxopts.hpp>

namespace borealis::cli {

// The game's own log file (ux0:/data/TwilitRealm/Dusklight/logs/dusklight-*.log) is flushed only for warnings and
// above by default, so a crash would lose the last information lines: the ones that say how far start-up got.
// Flush from Info up. Debug lines still wait for the next flush, which keeps per-frame debug logging cheap.
void StandardOptions::apply_to(log::Options& options) const {
    options.flushOn = LogLevel::Info;
}

void add_standard_options(cxxopts::Options&, const StandardOptionSet&) {}

StandardOptions parse(const cxxopts::ParseResult&) {
    return StandardOptions{};
}

}  // namespace borealis::cli
