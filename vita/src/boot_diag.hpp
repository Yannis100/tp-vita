// First-boot diagnosis: why the game cannot start, and a log that survives a crash. Header-only (POSIX and
// C++ only) so tests/host/disc can exercise it without the Vita SDK. The Vita-only parts (display, memory
// and firmware queries) are in vita/src/boot_screen.cpp and main.cpp.
//
// Why: the game's own fallback for a missing or unusable disc is the RmlUi pre-launch screen, which is
// stubbed on Vita, so the user would see a blank screen with no hint. main() checks the disc itself and
// shows these messages instead.
#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "boot_text.hpp"
#include "disc_probe.hpp"

namespace tpv {

// Size of a complete GameCube disc image (a mini-DVD). A plain .iso/.gcm that is shorter was usually cut
// short by an interrupted copy.
constexpr uint64_t kGameCubeDiscBytes = 1459978240ull;

// ---- what is in the disc folders -----------------------------------------------------------------------

enum class DiscVerdict : uint8_t { Usable, UnsupportedContainer, WrongGame, Unreadable };

struct DiscFile {
    std::string name;
    std::string path;
    uint64_t size = 0;
    DiscVerdict verdict = DiscVerdict::Unreadable;
    std::string gameId;    // 6 printable characters when the header could be read
    std::string container; // "RVZ", "WIA", ... for UnsupportedContainer
};

struct RootScan {
    std::string root; // as given, for display
    bool exists = false;
    std::vector<DiscFile> files; // every file with a disc-like extension, in name order
};

inline std::string upper_extension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
    return ext;
}

// Containers that hold a GameCube/Wii disc but that vita/nod_lite cannot read.
inline bool is_compressed_disc_extension(const std::string& upperExt) {
    return upperExt == "RVZ" || upperExt == "WIA" || upperExt == "GCZ" || upperExt == "WBFS" || upperExt == "NFS" ||
           upperExt == "TGC";
}

inline bool has_any_disc_extension(const std::string& name) {
    return has_disc_extension(name) || is_compressed_disc_extension(upper_extension(name));
}

// Names the container from the first bytes, whatever the extension says ("" = not a known container).
inline std::string sniff_container(const unsigned char head[4]) {
    if (std::memcmp(head, "RVZ\x01", 4) == 0) return "RVZ";
    if (std::memcmp(head, "WIA\x01", 4) == 0) return "WIA";
    if (std::memcmp(head, "WBFS", 4) == 0) return "WBFS";
    if (std::memcmp(head, "\x01\xC0\x0B\xB1", 4) == 0) return "GCZ";
    return {};
}

inline DiscFile classify_disc_file(const std::string& dir, const std::string& name) {
    DiscFile d;
    d.name = name;
    d.path = dir + "/" + name;
    struct stat st {};
    if (::stat(d.path.c_str(), &st) == 0) {
        d.size = static_cast<uint64_t>(st.st_size);
    }
    const std::string ext = upper_extension(name);
    if (is_compressed_disc_extension(ext)) {
        d.verdict = DiscVerdict::UnsupportedContainer;
        d.container = ext;
        return d;
    }
    FILE* f = std::fopen(d.path.c_str(), "rb");
    if (f == nullptr) {
        return d; // Unreadable
    }
    unsigned char head[4] = {};
    bool ok = std::fread(head, 1, 4, f) == 4;
    if (ok) {
        d.container = sniff_container(head);
        if (!d.container.empty()) {
            std::fclose(f);
            d.verdict = DiscVerdict::UnsupportedContainer;
            return d;
        }
    }
    const long discStart = (ok && std::memcmp(head, "CISO", 4) == 0) ? 0x8000 : 0;
    char id[6] = {};
    ok = ok && std::fseek(f, discStart, SEEK_SET) == 0 && std::fread(id, 1, 6, f) == 6;
    std::fclose(f);
    if (!ok) {
        return d; // Unreadable: empty or truncated
    }
    for (char c : id) {
        d.gameId += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    d.verdict = DiscVerdict::WrongGame;
    for (const char* accepted : {"GZ2E01", "GZ2J01", "GZ2P01"}) {
        if (d.gameId == accepted) {
            d.verdict = DiscVerdict::Usable;
        }
    }
    return d;
}

inline RootScan scan_root(const std::string& root) {
    RootScan scan;
    scan.root = root;
    std::string dir = root;
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return scan;
    }
    scan.exists = true;
    std::vector<std::string> names;
    while (const dirent* entry = readdir(d)) {
        if (has_any_disc_extension(entry->d_name)) {
            names.emplace_back(entry->d_name);
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        scan.files.push_back(classify_disc_file(dir, n));
    }
    return scan;
}

inline std::vector<RootScan> scan_roots(const std::vector<std::string>& roots) {
    std::vector<RootScan> out;
    for (const std::string& r : roots) {
        out.push_back(scan_root(r));
    }
    return out;
}

// One line saying what is wrong with a file (empty for a usable one).
inline std::string explain(const DiscFile& d) {
    switch (d.verdict) {
    case DiscVerdict::Usable:
        return {};
    case DiscVerdict::UnsupportedContainer:
        return d.container + " image: not readable here. Convert it to a plain .iso first (for example with Dolphin).";
    case DiscVerdict::WrongGame:
        if (!d.gameId.empty() && (d.gameId[0] == 'R' || d.gameId[0] == 'S')) {
            return "Wii disc (ID " + d.gameId + "): only the GameCube version (GZ2E01, GZ2P01, GZ2J01) works.";
        }
        return "game ID " + d.gameId + " is not Twilight Princess (GameCube: GZ2E01, GZ2P01, GZ2J01).";
    case DiscVerdict::Unreadable:
    default:
        return "cannot read the start of the file (empty, cut short, or no permission).";
    }
}

inline std::string format_size(uint64_t bytes) {
    char buf[64];
    if (bytes >= (1ull << 20)) {
        std::snprintf(buf, sizeof buf, "%llu bytes (%.1f MiB)", static_cast<unsigned long long>(bytes),
                      static_cast<double>(bytes) / (1 << 20));
    } else {
        std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// ---- screens -------------------------------------------------------------------------------------------

constexpr const char* kGameLogDir = "ux0:/data/TwilitRealm/Dusklight/logs/";
constexpr const char* kExitFooter = "Press O or X to exit.";

// No usable disc anywhere. `savedIso` is the disc path remembered in the game's config.json ("" if none);
// `savedProblem` says why it is not usable ("" if it was not checked).
inline Screen no_disc_screen(const std::vector<RootScan>& scans, const std::string& bootLogPath,
                             const std::string& savedIso, const std::string& savedProblem) {
    Screen s;
    s.title = "Twilight Princess: no disc image found";
    s.body.push_back({"Copy your own GameCube disc image of Twilight Princess to this folder on the Vita:", Style::Body});
    s.body.push_back({"  ux0:data/tpvita/", Style::Body});
    s.body.push_back({"A plain .iso, .gcm or .ciso image works if its game ID is GZ2E01 (USA), GZ2P01 (Europe) or "
                      "GZ2J01 (Japan). Start the game again afterwards.",
                      Style::Body});
    s.body.push_back({"", Style::Body});
    s.body.push_back({"Looked in:", Style::Body});
    for (const RootScan& r : scans) {
        std::string state;
        if (!r.exists) {
            state = "folder does not exist";
        } else if (r.files.empty()) {
            state = "no disc images";
        } else {
            state = std::to_string(r.files.size()) + (r.files.size() == 1 ? " file" : " files");
        }
        s.body.push_back({"  " + r.root + "  (" + state + ")", Style::Body});
        for (const DiscFile& f : r.files) {
            s.body.push_back({"    " + f.name + ": " + (f.verdict == DiscVerdict::Usable ? "usable" : explain(f)),
                              Style::Dim});
        }
    }
    if (!savedIso.empty()) {
        s.body.push_back({"", Style::Body});
        s.body.push_back({"The game's config.json also names a disc: " + savedIso, Style::Body});
        if (!savedProblem.empty()) {
            s.body.push_back({"  " + savedProblem, Style::Dim});
        }
    }
    s.body.push_back({"", Style::Body});
    s.body.push_back({"Log of this start: " + bootLogPath, Style::Dim});
    s.body.push_back({std::string("Game log: ") + kGameLogDir, Style::Dim});
    s.footer = kExitFooter;
    return s;
}

// The disc checks the game runs before it opens an image (dusk::iso::inspect), as a plain enum so that
// this header does not depend on the game code. main.cpp maps dusk::iso::ValidationError onto it.
enum class DiscCheck : uint8_t { Ok, IoError, InvalidImage, WrongGame, WrongVersion, Other };

inline const char* disc_check_text(DiscCheck c) {
    switch (c) {
    case DiscCheck::Ok:
        return "the image is fine";
    case DiscCheck::IoError:
        return "the file could not be opened or read.";
    case DiscCheck::InvalidImage:
        return "this is not a GameCube disc image that this build can read, or it is damaged.";
    case DiscCheck::WrongGame:
        return "this disc is not a Twilight Princess version the game accepts.";
    case DiscCheck::WrongVersion:
        return "this Twilight Princess version is not supported.";
    case DiscCheck::Other:
    default:
        return "the disc check failed for an unknown reason.";
    }
}

// A disc was found (or configured) but the game's own check rejects it.
inline Screen bad_disc_screen(const std::string& path, uint64_t size, DiscCheck check, const std::string& bootLogPath) {
    Screen s;
    s.title = "Twilight Princess: the disc image cannot be used";
    s.body.push_back({"File: " + path, Style::Body});
    s.body.push_back({"Size: " + format_size(size), Style::Body});
    s.body.push_back({std::string("Problem: ") + disc_check_text(check), Style::Body});
    s.body.push_back({"", Style::Body});
    const std::string ext = upper_extension(path);
    if (size != 0 && size < kGameCubeDiscBytes && (ext == "ISO" || ext == "GCM")) {
        s.body.push_back({"A complete GameCube disc image is " + std::to_string(kGameCubeDiscBytes) +
                              " bytes. This file is " + std::to_string(kGameCubeDiscBytes - size) +
                              " bytes shorter: the copy to the Vita was probably cut short. Copy it again "
                              "(a USB cable is more reliable than FTP) and compare the size.",
                          Style::Body});
    } else if (check == DiscCheck::InvalidImage) {
        s.body.push_back({"Only plain .iso/.gcm and .ciso images of the GameCube version are read. Wii images and "
                          "compressed formats (RVZ, WIA, GCZ, WBFS, NFS) are not. NKit-trimmed images may fail too.",
                          Style::Body});
    } else if (check == DiscCheck::IoError) {
        s.body.push_back({"Check that the file is not open in another program and that the memory card is healthy.",
                          Style::Body});
    }
    s.body.push_back({"", Style::Body});
    s.body.push_back({"Log of this start: " + bootLogPath, Style::Dim});
    s.body.push_back({std::string("Game log: ") + kGameLogDir, Style::Dim});
    s.footer = kExitFooter;
    return s;
}

// ---- which disc will the game open? ---------------------------------------------------------------------

// What main() found out. The checks themselves (file system, the game's own disc inspection) run in main();
// the decision is here so that every combination can be tested on the host.
struct DiscInputs {
    std::vector<RootScan> scans;
    std::string found;                         // first disc in the port's folders ("" = none)
    DiscCheck foundCheck = DiscCheck::Ok;      // the game's verdict on `found`
    uint64_t foundSize = 0;
    std::string saved;                         // disc path in the game's config.json ("" = none)
    bool savedExists = false;
    DiscCheck savedCheck = DiscCheck::Ok;      // the game's verdict on `saved` (only meaningful if savedExists)
    uint64_t savedSize = 0;
    std::string bootLogPath;
};

struct DiscDecision {
    bool ok = false;
    std::string inject; // a disc to pass as --dvd; empty when the game's saved path is used
    Screen screen;      // what to show when !ok
};

// Order: a disc in the port's folders that the game accepts (passed as --dvd), then the saved path in the
// game's config.json (the game picks it up by itself). Otherwise the screen names the first problem the user
// can act on: a disc they copied that is unusable, else a saved path that is unusable, else "no disc".
inline DiscDecision decide_disc(const DiscInputs& in) {
    DiscDecision d;
    if (!in.found.empty() && in.foundCheck == DiscCheck::Ok) {
        d.ok = true;
        d.inject = in.found;
        return d;
    }
    if (!in.saved.empty() && in.savedExists && in.savedCheck == DiscCheck::Ok) {
        d.ok = true;
        return d;
    }
    if (!in.found.empty()) {
        d.screen = bad_disc_screen(in.found, in.foundSize, in.foundCheck, in.bootLogPath);
    } else if (!in.saved.empty() && in.savedExists) {
        d.screen = bad_disc_screen(in.saved, in.savedSize, in.savedCheck, in.bootLogPath);
    } else {
        d.screen = no_disc_screen(in.scans, in.bootLogPath, in.saved, in.saved.empty() ? "" : "that file does not exist");
    }
    return d;
}

// ---- reading the game's saved disc path ------------------------------------------------------------------

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Finds `"key": "value"` in a JSON document and returns the unescaped value. The game's config.json is a
// flat object of dotted keys ("backend.isoPath"). This is a lookup for a diagnostic message, not a JSON
// parser: it returns false when the key is absent or its value is not a string.
inline bool extract_json_string(const std::string& text, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        size_t i = pos + needle.size();
        pos = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size() || text[i] != ':') {
            continue; // the text was a value, not a key
        }
        ++i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size() || text[i] != '"') {
            return false;
        }
        ++i;
        std::string value;
        while (i < text.size() && text[i] != '"') {
            if (text[i] != '\\') {
                value += text[i++];
                continue;
            }
            if (++i >= text.size()) {
                return false;
            }
            switch (text[i]) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'u': {
                if (i + 4 >= text.size()) {
                    return false;
                }
                uint32_t cp = 0;
                for (int k = 1; k <= 4; ++k) {
                    const char h = text[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
                    else return false;
                }
                append_utf8(value, cp);
                i += 4;
                break;
            }
            default: value += text[i]; break; // \" \\ \/
            }
            ++i;
        }
        if (i >= text.size()) {
            return false; // unterminated string
        }
        out = value;
        return true;
    }
    return false;
}

inline bool read_text_file(const std::string& path, std::string& out, size_t maxBytes = 1 << 20) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    char buf[4096];
    size_t n;
    while (out.size() < maxBytes && (n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

// ---- the boot log ----------------------------------------------------------------------------------------

inline bool make_dir(const std::string& path) {
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

// ux0:data/tpvita/boot.log: what main() saw and decided, one flushed line at a time, so the last line
// before a crash is on the card. The previous run's log is kept as boot.log.1. The game writes its own,
// larger log (kGameLogDir); this one covers what happens before that exists.
class BootLog {
public:
    BootLog() = default;
    BootLog(const BootLog&) = delete;
    BootLog& operator=(const BootLog&) = delete;
    ~BootLog() { close(); }

    bool open(const std::string& dir) {
        close();
        path_ = dir + "/boot.log";
        const std::string previous = path_ + ".1";
        std::remove(previous.c_str()); // rename() fails when the target exists on some Vita file systems
        std::rename(path_.c_str(), previous.c_str());
        file_ = std::fopen(path_.c_str(), "wb");
        t0_ = std::chrono::steady_clock::now();
        return file_ != nullptr;
    }

    void close() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }

    bool echo = true; // also write each line to stderr (a USB/net debug console shows it)

#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void line(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        if (echo) {
            std::fprintf(stderr, "[boot %8.3f] %s\n", t, buf);
        }
        if (file_ != nullptr) {
            std::fprintf(file_, "[%8.3f] %s\n", t, buf);
            std::fflush(file_);
        }
    }

private:
    FILE* file_ = nullptr;
    std::string path_;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

} // namespace tpv
