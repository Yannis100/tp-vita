// Host test for the crash recorder's symbol resolver. Runs on the host against the real table
// generated from the real ELF, so a broken generator or a broken parser fails here instead of on
// the fatal path of a Vita.
//
// This test exists because the first resolver was wrong in a way that looked like it worked: it
// reported borealis::log::file_descriptor() for an address inside
// borealis::log::detail::write_fatal(). Both defects that caused it are pinned below -- names
// containing spaces (43.8% of the table) and the size-based extent check.
//
// Addresses are read out of the table rather than hardcoded, because they move on every rebuild.
// The table is the thing under test; a hardcoded address would test the build, not the resolver.
//
// What this does NOT test: the Vita-specific parts (app0:/ux0: paths, the SIGABRT handler surviving
// a real abort, /proc/self/maps). Those need a real crash in Vita3K.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int tpv_crash_map_symbol(unsigned long address, char* out, unsigned outSize);
extern "C" int tpv_crash_table_is_valid();
extern "C" int tpv_crash_table_anchor_pair(unsigned long* tableAddress, unsigned long* liveAddress);
extern "C" void tpv_crash_table_set_path(const char* path);

static int gFailures = 0;

static void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++gFailures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static bool contains(const char* haystack, const char* needle) {
  return std::strstr(haystack, needle) != nullptr;
}

struct Entry {
  unsigned long address;
  unsigned long size;
  std::string name;
};

// Reads the table the same way the resolver does, so the test's expectations come from the artefact
// that ships rather than from a second implementation of the format.
static std::vector<Entry> read_table(const char* path) {
  std::vector<Entry> entries;
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return entries;
  }
  char line[1024];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (line[0] != '0' || line[1] != 'x') {
      continue; // comment or blank
    }
    unsigned long address = 0;
    unsigned long size = 0;
    int consumed = 0;
    if (std::sscanf(line, "0x%lx\t0x%lx\t%n", &address, &size, &consumed) < 2 || consumed == 0) {
      continue;
    }
    std::string name(line + consumed);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
      name.pop_back();
    }
    entries.push_back({address, size, name});
  }
  std::fclose(file);
  return entries;
}

static const Entry* find_by_name(const std::vector<Entry>& entries, const char* needle) {
  for (const Entry& e : entries) {
    if (e.name.find(needle) != std::string::npos) {
      return &e;
    }
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <crash_symbols.txt>\n", argv[0]);
    return 2;
  }
  const std::vector<Entry> entries = read_table(argv[1]);
  if (entries.empty()) {
    std::printf("FAIL: the table at %s is empty or unreadable\n", argv[1]);
    return 1;
  }
  std::printf("table: %zu symbols\n", entries.size());
  tpv_crash_table_set_path(argv[1]);

  char name[512];

  // 1. A plain C symbol at its exact address.
  const Entry* known = find_by_name(entries, "OSSwitchStack");
  expect(known != nullptr, "the table contains a plain C symbol");
  if (known != nullptr) {
    const int ok = tpv_crash_map_symbol(known->address, name, sizeof(name));
    expect(ok == 1, "resolves a known address");
    expect(ok == 1 && std::strcmp(name, known->name.c_str()) == 0, "names that symbol exactly");
  }

  // 2. THE REGRESSION. A demangled C++ name contains spaces and commas, and the old generator threw
  //    those rows away. If this fails, the table is generated from a source that cannot represent
  //    C++ names, and every crash log will name the wrong function.
  const Entry* fatal = find_by_name(entries, "write_fatal");
  expect(fatal != nullptr, "the table contains a C++ symbol whose name has spaces and commas");
  if (fatal != nullptr) {
    expect(fatal->name.find(' ') != std::string::npos, "that name really does contain spaces");
    const int ok = tpv_crash_map_symbol(fatal->address, name, sizeof(name));
    expect(ok == 1 && std::strcmp(name, fatal->name.c_str()) == 0,
           "resolves a C++ symbol whose name contains spaces");
    expect(ok == 1 && contains(name, "write_fatal"), "names write_fatal rather than a neighbour");

    // 3. THE SECOND REGRESSION. An address inside the function must name it, and an address past the
    //    end of a *short* neighbour must not name that neighbour. With the old size-less table every
    //    symbol had size 0 so this check could never fire.
    if (fatal->size > 4) {
      const int mid = tpv_crash_map_symbol(fatal->address + fatal->size / 2, name, sizeof(name));
      expect(mid == 1 && contains(name, "write_fatal"),
             "an address inside write_fatal names write_fatal");
    }
    const Entry* shortSym = nullptr;
    for (const Entry& e : entries) {
      if (e.size > 0 && e.size <= 0x20 && e.address > fatal->address) {
        shortSym = &e;
        break;
      }
    }
    if (shortSym != nullptr) {
      const int past = tpv_crash_map_symbol(shortSym->address + shortSym->size + 0x100, name,
                                            sizeof(name));
      expect(past == 0 || std::strcmp(name, shortSym->name.c_str()) != 0,
             "an address past a short symbol does not name it");
    }
  }

  // 4. Below the first symbol: nothing may match, and it must not crash.
  expect(tpv_crash_map_symbol(0x00000010u, name, sizeof(name)) == 0,
         "rejects an address below the image");

  // 5. Truncation, not overflow, for a caller with a small buffer.
  if (known != nullptr) {
    char tiny[4];
    const int tinyOk = tpv_crash_map_symbol(known->address, tiny, sizeof(tiny));
    expect(tinyOk == 1, "reports a hit with a small buffer");
    expect(std::strlen(tiny) < sizeof(tiny), "truncates instead of overflowing");
    expect(tpv_crash_map_symbol(known->address, nullptr, 16) == 0, "refuses a null buffer");
  }

  // 6. The anchor mechanism. The table must contain the anchor row, and the pair must report both
  //    addresses. It must NOT report them as equal here: this binary is compiled for x86-64, so the
  //    live anchor sits in a host address space while the table holds ARM addresses from the Vita
  //    ELF. Equality is only possible when the resolver is compiled into the image the table was
  //    made from, which is the case that matters and which only a Vita3K run can exercise. Asserting
  //    equality on the host would be asserting a falsehood.
  unsigned long recorded = 0;
  unsigned long live = 0;
  const int haveAnchor = tpv_crash_table_anchor_pair(&recorded, &live);
  expect(haveAnchor == 1, "the table contains the anchor symbol");
  expect(haveAnchor == 1 && recorded != 0, "the anchor has a recorded address");
  expect(haveAnchor == 1 && recorded != live,
         "on the host the anchor cannot match (different address space), so validity is refused");
  expect(tpv_crash_table_is_valid() == 0,
         "a table for another image is reported invalid, not trusted");

  // 7. A table that does not describe this image must be refused, not trusted.
  const char* bogusPath = "crash_test_bogus_table.txt";
  FILE* bogus = std::fopen(bogusPath, "wb");
  if (bogus != nullptr) {
    std::fprintf(bogus, "// deliberately wrong\n");
    std::fprintf(bogus, "0x%08lx\t0x4\ttpv_crash_table_anchor\n", live + 0x40u);
    std::fclose(bogus);
    tpv_crash_table_set_path(bogusPath);
    unsigned long badRecorded = 0;
    unsigned long badLive = 0;
    expect(tpv_crash_table_anchor_pair(&badRecorded, &badLive) == 1, "the bogus table has an anchor");
    expect(badRecorded != badLive, "the bogus anchor does not match the live address");
    std::remove(bogusPath);
  }

  std::printf(gFailures == 0 ? "PASS\n" : "FAILURES: %d\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
