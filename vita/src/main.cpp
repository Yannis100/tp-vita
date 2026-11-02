/**
 * Twilight Princess for PS Vita — entry point.
 *
 * The Dusklight game code provides `int aurora_main(int, char**)` (the macro in
 * <aurora/main.h> renames its `main`), which runs DuskMain -> game_main. The Vita
 * toolchain's crt0 exports `main` as the initial program entry, so this TU provides
 * the real `main` and forwards to aurora_main after configuring the runtime.
 *
 * Memory budget (365 MiB MAIN total with ATTRIBUTE2=12):
 *   - aurora_initialize clamps the game's requested MEM1 (256 MiB) to 64 MiB and
 *     MEM2 to 24 MiB (see vita/aurora/backend.cpp).
 *   - vitaGL carves its RAM pool out of the remaining free MAIN memory.
 *
 * First boot: before the game starts, main() writes ux0:data/tpvita/boot.log (what it saw and decided) and
 * checks that a usable disc image exists. If none does, it shows why on screen (boot_diag.hpp) instead of
 * letting the game wait forever on a blank screen.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <aurora/main.h>

#include "boot_diag.hpp"
#include "boot_screen.hpp"
#include "crash_record.hpp"
#include "disc_probe.hpp"

// dusk/settings.h (pulled in by iso_validate.hpp) expects the dolphin OS constants; the game gets them from its PCH.
#include <dolphin/os/OSRtc.h>
#include "dusk/iso_validate.hpp"

#include <sys/stat.h>

#include <cstring>
#include <string>
#include <vector>

#undef main

/* newlib heap: the big allocator backing malloc/new for the game code. */
unsigned int _newlib_heap_size_user = 220 * 1024 * 1024;

/* Sony libc heap (used by Sony libraries, not newlib malloc). */
unsigned int sceLibcHeapSize = 4 * 1024 * 1024;

/* Main-thread stack: deep decomp recursion crashes silently with the default. */
const unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

/* Lower = higher priority. */
const int sceUserMainThreadPriority = 0x60;

namespace {
// The port's own folder: the disc image goes here, and so does boot.log.
constexpr const char* kDataDir = "ux0:data/tpvita";
// The game's config file. SDL3's Vita backend puts the preference path at ux0:/data/<org>/<app>/ (the strings in
// libSDL3.a), and dusk/config.cpp names the file config.json; the key is "backend.isoPath".
constexpr const char* kGameConfig = "ux0:/data/TwilitRealm/Dusklight/config.json";
// Where vitaShaRK (ur0:/data/libshacccg.suprx) and vitaGL (ur0:data/external/...) look for Sony's shader compiler.
constexpr const char* kShaderCompilerPaths[] = {"ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"};

void set_clocks() {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

/**
 * The game only boots from a DVD image passed via --dvd or a saved isoPath, and its normal fallback
 * (the RmlUi pre-launch UI where the user picks the image) is stubbed on Vita. Without this, a first
 * launch with no saved isoPath would sit on a blank screen forever. So: if a GameCube disc image is
 * found at a conventional Vita location, inject `--dvd <path>` into argv before aurora_main runs.
 * What counts as a disc (formats vita/nod_lite can read, game IDs the game accepts) is in disc_probe.hpp.
 */
const std::vector<std::string>& disc_roots() {
    // ux0:data/tpvita/ is the port's userPath (vita/aurora/backend.cpp); keep the old tpvitaskel path working too.
    static const std::vector<std::string> roots = {"ux0:data/tpvita/", "ux0:data/tpvita/rom/",
                                                   "ux0:data/tpvitaskel/rom/"};
    return roots;
}

// cxxopts accepts `--dvd path` and `--dvd=path`.
bool has_dvd_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dvd") == 0 || std::strncmp(argv[i], "--dvd=", 6) == 0) {
            return true;
        }
    }
    return false;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

uint64_t file_size(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

// The check the game itself runs before it opens an image, so that what the screen says matches what the
// game would do with the same file.
tpv::DiscCheck check_disc(const std::string& path) {
    dusk::iso::DiscInfo info{};
    switch (dusk::iso::inspect(path.c_str(), info)) {
    case dusk::iso::ValidationError::Success:
        return tpv::DiscCheck::Ok;
    case dusk::iso::ValidationError::IOError:
        return tpv::DiscCheck::IoError;
    case dusk::iso::ValidationError::InvalidImage:
        return tpv::DiscCheck::InvalidImage;
    case dusk::iso::ValidationError::WrongGame:
        return tpv::DiscCheck::WrongGame;
    case dusk::iso::ValidationError::WrongVersion:
        return tpv::DiscCheck::WrongVersion;
    default:
        return tpv::DiscCheck::Other;
    }
}

void log_environment(tpv::BootLog& log, int argc, char* argv[]) {
    log.line("tp-vita boot, built %s %s", __DATE__, __TIME__);
    SceKernelSystemSwVersion fw{};
    fw.size = sizeof(fw);
    if (sceKernelGetSystemSwVersion(&fw) >= 0) {
        log.line("firmware %s", fw.versionString);
    }
    SceKernelFreeMemorySizeInfo mem{};
    mem.size = sizeof(mem);
    if (sceKernelGetFreeMemorySize(&mem) >= 0) {
        log.line("free memory at start: user %d MiB, cdram %d MiB, phycont %d MiB", mem.size_user >> 20,
                 mem.size_cdram >> 20, mem.size_phycont >> 20);
    }
    for (int i = 0; i < argc; ++i) {
        log.line("argv[%d] = %s", i, argv[i]);
    }
    // Real hardware needs Sony's shader compiler, which the user extracts from their own console; it is not
    // in the VPK. Vita3K may run without it, so a missing file is a warning here, not a stop.
    bool haveCompiler = false;
    for (const char* path : kShaderCompilerPaths) {
        const bool present = file_exists(path);
        haveCompiler = haveCompiler || present;
        log.line("shader compiler %s: %s", path, present ? "found" : "not found");
    }
    if (!haveCompiler) {
        log.line("WARNING: libshacccg.suprx is missing. On a real Vita every shader compile will fail (black screen); "
                 "Vita3K may not need it.");
    }
}

// Looks at the disc folders and the game's saved disc path, logs what it finds, and decides (tpv::decide_disc).
tpv::DiscDecision check_discs(tpv::BootLog& log) {
    tpv::DiscInputs in;
    in.bootLogPath = log.path();
    in.scans = tpv::scan_roots(disc_roots());
    for (const tpv::RootScan& s : in.scans) {
        // size_t is cast: the VitaSDK's newlib has no z length modifier (the log read "zu disc-like file(s)")
        log.line("folder %s: %s, %u disc-like file(s)", s.root.c_str(), s.exists ? "present" : "missing",
                 static_cast<unsigned>(s.files.size()));
        for (const tpv::DiscFile& f : s.files) {
            log.line("  %s  %llu bytes  id=%s  %s", f.name.c_str(), static_cast<unsigned long long>(f.size),
                     f.gameId.empty() ? "-" : f.gameId.c_str(),
                     f.verdict == tpv::DiscVerdict::Usable ? "looks usable" : tpv::explain(f).c_str());
        }
    }

    if (tpv::find_disc_image(disc_roots(), in.found)) {
        in.foundCheck = check_disc(in.found);
        in.foundSize = file_size(in.found);
        log.line("disc image %s: %s", in.found.c_str(), tpv::disc_check_text(in.foundCheck));
    }

    std::string config;
    if (tpv::read_text_file(kGameConfig, config) && tpv::extract_json_string(config, "backend.isoPath", in.saved) &&
        !in.saved.empty()) {
        log.line("config.json names disc %s", in.saved.c_str());
        in.savedExists = file_exists(in.saved);
        if (in.savedExists) {
            in.savedCheck = check_disc(in.saved);
            in.savedSize = file_size(in.saved);
            log.line("  the game's check: %s", tpv::disc_check_text(in.savedCheck));
        } else {
            log.line("  that file does not exist");
        }
    } else {
        in.saved.clear();
        log.line("config.json (%s): no saved disc path", kGameConfig);
    }
    return tpv::decide_disc(in);
}
} // namespace

extern "C" int main(int argc, char* argv[]) {
    // First, before anything that can abort. A silent abort is what cost two sessions here: the
    // address resolved to _kill_r, so the process chose to die, and nothing recorded why.
    tpv_crash_install();

    set_clocks();

    // The folder the user is told to fill: create it so that it exists after the first launch.
    tpv::make_dir(kDataDir);
    static tpv::BootLog log; // static: the log stays open while the game runs
    log.open(kDataDir);
    log_environment(log, argc, argv);

    // Inject --dvd <disc> if a disc image exists and the caller did not already pass one.
    std::string disc;
    if (has_dvd_arg(argc, argv)) {
        log.line("--dvd was passed on the command line: skipping the disc search");
    } else {
        tpv::DiscDecision decision = check_discs(log);
        if (!decision.ok) {
            log.line("no usable disc: showing the error screen");
            const bool shown = tpv::show_screen_and_wait(decision.screen);
            log.line(shown ? "error screen dismissed, exiting" : "could not allocate display memory for the error screen, exiting");
            log.close();
            sceKernelExitProcess(0);
            return 0;
        }
        disc = decision.inject;
    }

    if (!disc.empty()) {
        // The strings and the vector must outlive aurora_main.
        static std::string sFlag = "--dvd";
        static std::string sDisc = disc;
        static std::vector<const char*> sArgv;
        sArgv.assign(argv, argv + argc);
        sArgv.push_back(sFlag.c_str());
        sArgv.push_back(sDisc.c_str());
        sArgv.push_back(nullptr);
        argc = static_cast<int>(sArgv.size()) - 1;
        argv = const_cast<char**>(sArgv.data());
        log.line("starting the game with --dvd %s", disc.c_str());
    } else {
        log.line("starting the game");
    }

    const int rc = aurora_main(argc, argv);
    log.line("aurora_main returned %d", rc);
    return rc;
}
