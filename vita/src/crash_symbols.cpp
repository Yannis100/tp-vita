// Resolve an address to a symbol using a table generated from the linked ELF.
//
// Why a file and not an embedded table. The first version of this compiled the whole symbol table
// into .rodata as a string literal. That was wrong for two independent reasons, both measured:
//
//   1. It could not be correct. Any *complete* text-symbol table for this image is about 4.2 MiB,
//      and the read-only segment has 118 KiB of slack before the data segment, so the table had to
//      be cut down. The cut was silent: the generator dropped every name containing a space (43.8%
//      of them) and every size, because the linker map it read from records neither. An address
//      inside borealis::log::detail::write_fatal() therefore resolved to the nearest *surviving*
//      symbol, borealis::log::file_descriptor(), which is a 0x10-byte atomic load that cannot
//      abort. The recorder named innocent code, and that cost a session.
//   2. It was one build stale by construction. The table was generated after linking, so it could
//      only be compiled into the next link, and re-adding it changed the layout it described.
//
// A file has neither problem. It is produced from the ELF that was just linked, so it describes
// exactly the image that runs, and it costs no .rodata and no RAM when a run is healthy -- it is
// read only when the process is already dying. When the table is absent the recorder reports raw
// addresses, which is less useful than a name and far more useful than a wrong name.
//
// The table is produced by tools/map_symbols.py. One row per symbol, TAB separated (tab cannot
// occur in a demangled name, but commas occur in 19,226 of them):
//     0x81c7f700	0x15a	borealis::log::detail::write_fatal(std::basic_string_view<...>, ...)
//
// Staleness is still possible -- copy a new table next to an old build and the addresses are from a
// different image -- so the table carries the address of tpv_crash_table_anchor, a function defined
// here. tpv_crash_table_is_valid() compares that recorded address with the live one. If they differ
// the table does not describe this image, and the recorder says so and prints raw addresses instead
// of a plausible lie.
#include "crash_record.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// app0: is the VPK itself and is readable on hardware and in Vita3K, so the table ships inside the
// VPK and there is nothing for the user to copy. ux0:data is second because it needs no rebuild to
// update, which matters when a table must be refreshed for an already-installed build.
constexpr const char* kCandidates[] = {
    "app0:data/tpvita/crash_symbols.txt",
    "ux0:data/tpvita/crash_symbols.txt",
};
constexpr unsigned kNameCap = 192;
constexpr unsigned kLineCap = 512;

const char* gOverride = nullptr; // set by the host test; nullptr means "try kCandidates in order"

struct Row {
  uintptr_t address;
  uintptr_t size;
  char name[kNameCap];
};

// 0..15 for a hex digit, 0xff for anything else. Avoids strtoul, which is locale-aware and not
// async-signal-safe.
inline unsigned hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<unsigned>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<unsigned>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<unsigned>(c - 'A' + 10);
  }
  return 0xffu;
}

// Parses "0xADDR\t0xSIZE\tNAME" from a NUL-terminated line. Returns false for a comment, a blank
// line or anything malformed, so a damaged table degrades to "no name" rather than a bad one.
bool parse_line(const char* line, Row& out) {
  const char* p = line;
  if (p[0] != '0' || p[1] != 'x') {
    return false;
  }
  p += 2;
  uintptr_t address = 0;
  while (*p != '\0' && *p != '\t') {
    const unsigned digit = hex_digit(*p);
    if (digit > 15u) {
      return false;
    }
    address = (address << 4) | digit;
    ++p;
  }
  if (*p != '\t') {
    return false;
  }
  ++p;
  if (p[0] != '0' || p[1] != 'x') {
    return false;
  }
  p += 2;
  uintptr_t size = 0;
  while (*p != '\0' && *p != '\t') {
    const unsigned digit = hex_digit(*p);
    if (digit > 15u) {
      return false;
    }
    size = (size << 4) | digit;
    ++p;
  }
  if (*p != '\t' || p[1] == '\0') {
    return false;
  }
  ++p;
  out.address = address;
  out.size = size;
  unsigned n = 0;
  while (p[n] != '\0' && p[n] != '\n' && n + 1 < kNameCap) {
    out.name[n] = p[n];
    ++n;
  }
  out.name[n] = '\0';
  return n != 0;
}

// Streams one open table once, keeping the nearest row at or below `target`. Nothing is buffered
// for the file itself, so a 4 MiB table costs a 512-byte line buffer and no allocation. If
// `anchorOut` is non-null it also returns the table's recorded address for tpv_crash_table_anchor.
void scan_file(FILE* file, uintptr_t target, Row& best, bool& found, uintptr_t* anchorOut) {
  found = false;
  char line[kLineCap];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    const size_t length = std::strlen(line);
    if (length + 1 >= sizeof(line) && line[length - 1] != '\n') {
      // A name too long for the buffer: skip the remainder so the next fgets starts on a fresh row,
      // rather than parsing the tail of this one as if it were a row.
      while (std::fgets(line, sizeof(line), file) != nullptr) {
        const size_t tail = std::strlen(line);
        if (tail > 0 && line[tail - 1] == '\n') {
          break;
        }
      }
      continue;
    }
    Row row;
    if (!parse_line(line, row)) {
      continue;
    }
    if (anchorOut != nullptr && std::strcmp(row.name, "tpv_crash_table_anchor") == 0) {
      *anchorOut = row.address;
    }
    if (row.address <= target && (!found || row.address >= best.address)) {
      best = row;
      found = true;
    }
  }
}

// Opens the first table that exists and scans it. Returns false if no table could be opened at all.
bool scan(uintptr_t target, Row& best, bool& found, uintptr_t* anchorOut) {
  found = false;
  if (gOverride != nullptr) {
    FILE* file = std::fopen(gOverride, "rb");
    if (file == nullptr) {
      return false;
    }
    scan_file(file, target, best, found, anchorOut);
    std::fclose(file);
    return true;
  }
  for (const char* path : kCandidates) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
      continue;
    }
    scan_file(file, target, best, found, anchorOut);
    std::fclose(file);
    return true;
  }
  return false;
}

// The live address of the anchor. extern "C" so its name in the table is exactly this identifier,
// and referenced by tpv_crash_table_is_valid() so the linker cannot discard it.
extern "C" {
__attribute__((used, noinline)) void tpv_crash_table_anchor() {}
}

} // namespace

extern "C" {

// Copies the symbol name for `address` into `out`, truncated to `outSize`. Returns 1 on a match, 0
// otherwise (no table, no match, or an address past the end of its symbol).
int tpv_crash_map_symbol(uintptr_t address, char* out, unsigned outSize) {
  if (out == nullptr || outSize == 0) {
    return 0;
  }
  Row best;
  best.address = 0;
  best.size = 0;
  best.name[0] = '\0';
  bool found = false;
  if (!scan(address, best, found, nullptr) || !found) {
    return 0;
  }
  // Reject an address past the end of the nearest symbol. Sizes are real now (they come from nm),
  // so this check actually fires; the old size-less table could never trigger it.
  if (best.size != 0 && address >= best.address + best.size) {
    return 0;
  }
  unsigned n = 0;
  while (best.name[n] != '\0' && n + 1 < outSize) {
    out[n] = best.name[n];
    ++n;
  }
  out[n] = '\0';
  return n != 0 ? 1 : 0;
}

// ARM Thumb: a code address in a pointer has bit 0 set, but a symbol's value in the ELF symbol table
// (what nm prints) has it clear. On this build the two differed by exactly 1 -- table 0x8108f570 vs
// live 0x8108f571 -- and the validity check reported a mismatch on a table that was in fact correct.
// Found on the first boot after the check was added, which is the argument for checking every boot
// instead of only when something has already gone wrong.
//
// Clearing bit 0 is safe here and only here: the anchor is a function, code is at least 2-byte
// aligned on this target, so bit 0 carries no address information.
inline uintptr_t code_address(uintptr_t address) {
  return address & ~static_cast<uintptr_t>(1);
}

int tpv_crash_table_anchor_pair(uintptr_t* tableAddress, uintptr_t* liveAddress) {
  if (tableAddress == nullptr || liveAddress == nullptr) {
    return 0;
  }
  Row ignored;
  ignored.address = 0;
  ignored.size = 0;
  ignored.name[0] = '\0';
  bool found = false;
  uintptr_t recorded = 0;
  if (!scan(0, ignored, found, &recorded)) {
    return 0; // no table at all
  }
  *tableAddress = code_address(recorded);
  *liveAddress = code_address(reinterpret_cast<uintptr_t>(&tpv_crash_table_anchor));
  return recorded != 0 ? 1 : 0;
}

// 1 if a name from this table is safe to trust for this run, 0 if it is not.
//
// Deliberately not cached. The check is one pass over a 5 MB file, which is milliseconds on a path
// that runs once while dying, and caching made the answer depend on call order -- which is exactly
// the kind of hidden state that turns a diagnostic into a second bug.
int tpv_crash_table_is_valid() {
  uintptr_t recorded = 0;
  uintptr_t live = 0;
  if (tpv_crash_table_anchor_pair(&recorded, &live) == 0) {
    return 0;
  }
  return recorded == live ? 1 : 0;
}

// Test seam: point the resolver at a table built elsewhere. Not called by the game.
void tpv_crash_table_set_path(const char* path) {
  gOverride = path;
}

} // extern "C"
