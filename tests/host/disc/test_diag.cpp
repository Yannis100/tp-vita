// Host test for the first-boot diagnosis code: vita/src/{boot_font,boot_text,boot_diag}.hpp and the shader
// source dump the GL replay writes when a program fails to compile (vita/aurora/overlay/gfx/vita_shader_dump.hpp).
//   tests/host/disc/run.sh
//
//   TPV_SCREEN_DIR=/tmp/screens tests/host/disc/run.sh   also writes the error screens as PPM files
//   TPV_DUMP_FONT=1 tests/host/disc/run.sh               prints every glyph as ASCII art
#include "boot_diag.hpp"
#include "vita_shader_dump.hpp"  // vita/aurora/overlay/gfx

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                       \
  do {                                                    \
    ++g_checks;                                           \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
    }                                                     \
  } while (0)

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static std::string join(const std::vector<tpv::Line>& lines) {
  std::string s;
  for (const auto& l : lines) s += l.text + "\n";
  return s;
}
static std::string join(const tpv::Screen& screen) {
  std::string s = screen.title + "\n";
  for (const auto& l : screen.body) s += l.text + "\n";
  return s + screen.footer + "\n";
}

static void write_file(const std::string& path, const std::string& bytes, size_t padTo = 0) {
  FILE* f = std::fopen(path.c_str(), "wb");
  std::string data = bytes;
  if (data.size() < padTo) data.resize(padTo, '\0');
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

// ---- font ----------------------------------------------------------------------------------------------

static bool row_has_ink(const uint8_t* g, int row) { return g[row] != 0; }

static void test_font() {
  using namespace tpv;
  std::set<std::string> seen;
  for (int c = kFirstGlyph; c < kFirstGlyph + kGlyphCount; ++c) {
    const uint8_t* g = glyph_for(static_cast<unsigned char>(c));
    std::string key;
    bool ink = false;
    for (int r = 0; r < kGlyphH; ++r) {
      CHECK(g[r] < (1u << kGlyphW));  // only 5 columns are used
      key += static_cast<char>(g[r]);
      ink = ink || g[r] != 0;
    }
    if (c == ' ') {
      CHECK(!ink);
    } else {
      CHECK(ink);
      CHECK(seen.insert(key).second);  // no two glyphs alike (catches a copy-paste slip in the table)
    }
    if (std::getenv("TPV_DUMP_FONT") != nullptr) {
      std::printf("'%c'\n", c);
      for (int r = 0; r < kGlyphH; ++r) {
        for (int x = 0; x < kGlyphW; ++x) std::putchar((g[r] >> (kGlyphW - 1 - x)) & 1 ? '#' : '.');
        std::putchar('\n');
      }
    }
  }
  // Typographic intent, stated independently of the table: capitals and digits sit on rows 0-6 and leave
  // row 7 empty; the descenders g j p q y (and the comma) reach row 7.
  for (const char* s : {"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}) {
    for (const char* p = s; *p; ++p) CHECK(!row_has_ink(glyph_for(*p), 7));
  }
  for (char c : {'g', 'j', 'p', 'q', 'y', ','}) CHECK(row_has_ink(glyph_for(c), 7));
  for (char c : {'a', 'c', 'e', 'm', 'n', 'o', 'r', 's', 'u', 'v', 'w', 'x', 'z'}) {
    CHECK(!row_has_ink(glyph_for(c), 0) && !row_has_ink(glyph_for(c), 1));  // x-height letters start at row 2
    CHECK(!row_has_ink(glyph_for(c), 7));
  }
  for (char c : {'b', 'd', 'f', 'h', 'k', 'l', 't'}) CHECK(row_has_ink(glyph_for(c), 0) || row_has_ink(glyph_for(c), 1));
  // Characters outside printable ASCII (a UTF-8 file name) are drawn as '?'.
  CHECK(glyph_for(0x80) == glyph_for('?'));
  CHECK(glyph_for(0xC3) == glyph_for('?'));
  CHECK(glyph_for(0x1F) == glyph_for('?'));
  CHECK(glyph_for(0x7F) == glyph_for('?'));
  // Letters that are easy to confuse differ in more than one pixel.
  auto differs = [](char a, char b) {
    int d = 0;
    for (int r = 0; r < kGlyphH; ++r) d += __builtin_popcount(glyph_for(a)[r] ^ glyph_for(b)[r]);
    return d;
  };
  CHECK(differs('0', 'O') >= 2 && differs('1', 'l') >= 2 && differs('1', 'I') >= 2 && differs('I', 'l') >= 2);
  CHECK(differs('5', 'S') >= 2 && differs('2', 'Z') >= 2 && differs('8', 'B') >= 2 && differs('u', 'v') >= 2);
}

// ---- drawing -------------------------------------------------------------------------------------------

static void test_draw() {
  using namespace tpv;
  const uint32_t bg = rgb(1, 2, 3), fg = rgb(250, 240, 230);
  CHECK(bg == 0xFF030201u);  // A8B8G8R8: a u32 is 0xAABBGGRR

  // 'T' at scale 1 with a pitch wider than the width: pitch must be honoured, nothing outside is touched.
  const int W = 20, H = 12, P = 24;
  std::vector<uint32_t> buf(static_cast<size_t>(P) * H, 0xDEADBEEFu);
  Canvas c{buf.data(), W, H, P};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) buf[y * P + x] = bg;
  draw_glyph(c, 2, 1, 'T', fg, 1);
  // T: row 0 is five pixels, rows 1..6 only the middle column.
  for (int x = 0; x < 5; ++x) CHECK(buf[1 * P + 2 + x] == fg);
  for (int r = 1; r < 7; ++r) {
    for (int x = 0; x < 5; ++x) CHECK(buf[(1 + r) * P + 2 + x] == (x == 2 ? fg : bg));
  }
  for (int x = 0; x < 5; ++x) CHECK(buf[(1 + 7) * P + 2 + x] == bg);
  CHECK(buf[1 * P + 1] == bg && buf[1 * P + 7] == bg);          // neighbours untouched
  for (int y = 0; y < H; ++y)
    for (int x = W; x < P; ++x) CHECK(buf[y * P + x] == 0xDEADBEEFu);  // padding untouched

  // 'L' is not mirror-symmetric (a left column and a bottom bar): a mirrored or flipped glyph must fail here.
  {
    std::vector<uint32_t> bl(static_cast<size_t>(8) * 10, bg);
    Canvas cL{bl.data(), 8, 10, 8};
    draw_glyph(cL, 1, 1, 'L', fg, 1);
    for (int r = 0; r < 6; ++r)
      for (int x = 0; x < 5; ++x) CHECK(bl[(1 + r) * 8 + 1 + x] == (x == 0 ? fg : bg));
    for (int x = 0; x < 5; ++x) CHECK(bl[(1 + 6) * 8 + 1 + x] == fg);
    for (int x = 0; x < 5; ++x) CHECK(bl[(1 + 7) * 8 + 1 + x] == bg);
  }

  // Scale 2: every glyph pixel becomes a 2x2 block.
  std::vector<uint32_t> b2(static_cast<size_t>(24) * 20, bg);
  Canvas c2{b2.data(), 24, 20, 24};
  draw_glyph(c2, 0, 0, 'T', fg, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 10; ++x) CHECK(b2[y * 24 + x] == fg);  // top bar: 5 px * 2
  for (int y = 2; y < 14; ++y)
    for (int x = 0; x < 10; ++x) CHECK(b2[y * 24 + x] == ((x == 4 || x == 5) ? fg : bg));

  // Clipping: glyphs partly or wholly outside the canvas must not write out of bounds (ASan checks).
  std::vector<uint32_t> small(8 * 8, bg);
  Canvas cs{small.data(), 8, 8, 8};
  draw_text(cs, -7, -3, "Hello, world", fg, 3);
  draw_text(cs, 6, 6, "Hello", fg, 2);
  draw_text(cs, 100, 100, "off", fg, 1);
  draw_text(cs, -100, -100, "off", fg, 1);
  CHECK(true);

  // Text advances by one cell per character.
  std::vector<uint32_t> line(static_cast<size_t>(kCellW) * 3 * kGlyphH, bg);
  Canvas cl{line.data(), kCellW * 3, kGlyphH, kCellW * 3};
  draw_text(cl, 0, 0, "II", fg, 1);
  // 'I' = 01110 on row 0 -> columns 1..3 of each cell.
  CHECK(line[0] == bg && line[1] == fg && line[3] == fg && line[4] == bg && line[5] == bg);
  CHECK(line[kCellW + 1] == fg && line[kCellW + 3] == fg);
  CHECK(line[2 * kCellW + 1] == bg);  // third cell is empty
}

// ---- wrapping and layout -------------------------------------------------------------------------------

static std::string squash(const std::string& s) {
  std::string out;
  for (char c : s)
    if (c != ' ' && c != '\n') out += c;
  return out;
}

static void test_wrap() {
  using namespace tpv;
  CHECK(wrap_line("", 10) == std::vector<std::string>{""});
  CHECK(wrap_line("short", 10) == std::vector<std::string>{"short"});
  CHECK(wrap_line("exactly ten", 11) == std::vector<std::string>{"exactly ten"});
  // break at a space, continuation indented two deeper than the line's own indent
  CHECK((wrap_line("  alpha beta gamma", 12) == std::vector<std::string>{"  alpha beta", "    gamma"}));
  CHECK((wrap_line("alpha beta gamma", 10) == std::vector<std::string>{"alpha beta", "  gamma"}));
  // a long path with no spaces breaks after a '/'
  CHECK((wrap_line("ux0:data/tpvita/Legend.iso", 17) == std::vector<std::string>{"ux0:data/tpvita/", "  Legend.iso"}));
  // a word longer than the width is cut in the middle
  CHECK((wrap_line("abcdefghijklmnop", 8) == std::vector<std::string>{"abcdefgh", "  ijklmn", "  op"}));
  // zero width does not loop forever
  CHECK(wrap_line("abc", 0).size() == 1);

  // Property: rows never exceed the width, and no character is lost or invented.
  std::mt19937 rng(7);
  const char alphabet[] = "ab /./: _-xyz";
  for (int iter = 0; iter < 3000; ++iter) {
    std::string line(rng() % 90, ' ');
    for (char& ch : line) ch = alphabet[rng() % (sizeof alphabet - 1)];
    const size_t cols = 1 + rng() % 40;
    const auto rows = wrap_line(line, cols);
    std::string joined;
    bool tooWide = false;
    for (const auto& r : rows) {
      tooWide = tooWide || r.size() > cols;
      joined += r;
    }
    CHECK(!tooWide);
    CHECK(squash(joined) == squash(line));
    CHECK(rows.size() >= 1);
  }
}

static void test_layout() {
  using namespace tpv;
  Screen s;
  s.title = "Title";
  for (int i = 0; i < 4; ++i) s.body.push_back({"line " + std::to_string(i), Style::Body});
  s.footer = "Press O or X to exit.";
  auto rows = layout(s, 40, 14);
  CHECK(rows.size() == 14);
  CHECK(rows[0].text == "Title" && rows[0].style == Style::Title);
  CHECK(rows[1].text.empty());
  CHECK(rows[2].text == "line 0" && rows[5].text == "line 3");
  CHECK(rows.back().text == s.footer && rows.back().style == Style::Footer);
  CHECK(rows[12].text.empty());  // padding between body and footer

  // too much body: cut, marked, footer still on the last row
  Screen big = s;
  for (int i = 0; i < 100; ++i) big.body.push_back({"more " + std::to_string(i), Style::Body});
  rows = layout(big, 40, 14);
  CHECK(rows.size() == 14);
  CHECK(rows.back().text == s.footer);
  // layout: title, spacer, 9 body rows, the marker, a spacer, the footer
  CHECK(contains(rows[11].text, "more lines") && contains(rows[11].text, "boot.log"));
  CHECK(rows[12].text.empty());
  {
    int shown = 0;  // the count in the marker is exactly what was cut
    for (size_t i = 2; i < 11; ++i)
      if (!rows[i].text.empty()) ++shown;
    CHECK(shown == 9);
    CHECK(contains(rows[11].text, std::to_string(big.body.size() - static_cast<size_t>(shown)) + " more"));
  }

  // tiny screens never overflow
  for (int r = 1; r < 8; ++r) CHECK(layout(big, 40, r).size() <= static_cast<size_t>(r));
  // wide lines wrap inside the layout and keep their style
  Screen w;
  w.title = "T";
  w.body.push_back({"aaaa bbbb cccc dddd", Style::Dim});
  w.footer = "F";
  rows = layout(w, 9, 10);
  CHECK(rows.size() == 10 && rows[2].style == Style::Dim && rows[3].style == Style::Dim);
  CHECK(squash(join(rows)).find("aaaabbbbccccdddd") != std::string::npos);
}

// ---- disc scan and screens -----------------------------------------------------------------------------

static void test_scan(const std::string& tmp) {
  using namespace tpv;
  const std::string d = tmp + "/scan";
  mkdir(d.c_str(), 0755);
  write_file(d + "/GZ2E01.iso", "GZ2E01", 0x9000);
  write_file(d + "/Zelda PAL.GCM", "GZ2P01", 0x9000);
  write_file(d + "/mario.iso", "GM4E01", 0x9000);
  write_file(d + "/wii.iso", "RZDE01", 0x9000);
  write_file(d + "/dolphin.rvz", "whatever", 0x100);
  write_file(d + "/disguised.iso", std::string("RVZ\x01rest", 8), 0x9000);
  write_file(d + "/wbfs-ish.iso", "WBFSxxxx", 0x9000);
  write_file(d + "/gcz-ish.iso", std::string("\x01\xC0\x0B\xB1xxxx", 8), 0x9000);
  write_file(d + "/empty.iso", "", 0);
  write_file(d + "/tiny.iso", "GZ", 0);
  write_file(d + "/notes.txt", "GZ2E01", 0x9000);
  write_file(d + "/locked.iso", "GZ2E01", 0x9000);
  chmod((d + "/locked.iso").c_str(), 0000);
  write_file(d + "/tp.ciso", "CISO", 0x8000);
  {
    FILE* f = std::fopen((d + "/tp.ciso").c_str(), "r+b");
    std::fseek(f, 0x8000, SEEK_SET);
    std::fwrite("GZ2J01", 1, 6, f);
    std::fclose(f);
  }

  const RootScan s = scan_root(d + "/");
  CHECK(s.exists);
  std::set<std::string> names;
  for (const auto& f : s.files) names.insert(f.name);
  CHECK(names.count("notes.txt") == 0);  // not a disc extension
  CHECK(s.files.size() == 12);
  CHECK(std::is_sorted(s.files.begin(), s.files.end(), [](const DiscFile& a, const DiscFile& b) { return a.name < b.name; }));

  auto get = [&](const std::string& n) -> const DiscFile& {
    for (const auto& f : s.files)
      if (f.name == n) return f;
    static DiscFile none;
    CHECK(false);
    return none;
  };
  CHECK(get("GZ2E01.iso").verdict == DiscVerdict::Usable && get("GZ2E01.iso").gameId == "GZ2E01");
  CHECK(get("Zelda PAL.GCM").verdict == DiscVerdict::Usable);  // extension in upper case
  CHECK(get("tp.ciso").verdict == DiscVerdict::Usable && get("tp.ciso").gameId == "GZ2J01");
  CHECK(get("mario.iso").verdict == DiscVerdict::WrongGame && get("mario.iso").gameId == "GM4E01");
  CHECK(get("wii.iso").verdict == DiscVerdict::WrongGame);
  CHECK(contains(explain(get("wii.iso")), "Wii disc"));
  CHECK(contains(explain(get("mario.iso")), "GM4E01"));
  CHECK(get("dolphin.rvz").verdict == DiscVerdict::UnsupportedContainer && get("dolphin.rvz").container == "RVZ");
  CHECK(get("disguised.iso").verdict == DiscVerdict::UnsupportedContainer && get("disguised.iso").container == "RVZ");
  CHECK(get("wbfs-ish.iso").container == "WBFS");
  CHECK(get("gcz-ish.iso").container == "GCZ");
  CHECK(contains(explain(get("dolphin.rvz")), "RVZ") && contains(explain(get("dolphin.rvz")), "plain .iso"));
  CHECK(get("empty.iso").verdict == DiscVerdict::Unreadable);
  CHECK(get("tiny.iso").verdict == DiscVerdict::Unreadable);
  CHECK(explain(get("GZ2E01.iso")).empty());
  CHECK(get("GZ2E01.iso").size == 0x9000);
  if (geteuid() != 0) CHECK(get("locked.iso").verdict == DiscVerdict::Unreadable);  // root can read anything

  // a missing folder and an empty one
  CHECK(!scan_root(tmp + "/nope").exists);
  mkdir((tmp + "/emptydir").c_str(), 0755);
  CHECK(scan_root(tmp + "/emptydir").exists && scan_root(tmp + "/emptydir").files.empty());
  // a non-printable ID is shown with '?', never raw bytes
  write_file(d + "/bin.iso", std::string("\x01\x02\xFFxyz", 6), 0x9000);
  CHECK(scan_root(d).files.size() == 13);
  for (const auto& f : scan_root(d).files)
    if (f.name == "bin.iso") CHECK(f.gameId == "???xyz");

  chmod((d + "/locked.iso").c_str(), 0644);
}

static void test_screens(const std::string& tmp) {
  using namespace tpv;
  const std::string a = tmp + "/s1", b = tmp + "/s2";
  mkdir(a.c_str(), 0755);
  write_file(a + "/mario.iso", "GM4E01", 0x9000);
  write_file(a + "/zelda.rvz", "x", 0x100);
  const auto scans = scan_roots({a + "/", b});
  const Screen none = no_disc_screen(scans, "ux0:data/tpvita/boot.log", "", "");
  const std::string text = join(none);
  CHECK(contains(text, "no disc image found"));
  CHECK(contains(text, "ux0:data/tpvita/"));        // where to put the disc
  CHECK(contains(text, "GZ2E01") && contains(text, "GZ2P01") && contains(text, "GZ2J01"));
  CHECK(contains(text, a + "/  (2 files)"));        // each root and what it holds
  CHECK(contains(text, b + "  (folder does not exist)"));
  CHECK(contains(text, "mario.iso: game ID GM4E01"));
  CHECK(contains(text, "zelda.rvz: RVZ image"));
  CHECK(contains(text, "boot.log") && contains(text, kGameLogDir));
  CHECK(none.footer == kExitFooter);
  CHECK(!contains(text, "config.json"));            // no saved path: no mention

  const Screen saved = no_disc_screen(scans, "L", "uma0:iso/tp.iso", "the file does not exist");
  CHECK(contains(join(saved), "uma0:iso/tp.iso") && contains(join(saved), "the file does not exist"));

  // bad disc: short plain image -> the size hint carries the exact shortfall
  const uint64_t size = 600ull * 1024 * 1024;
  const Screen shortScreen = bad_disc_screen("ux0:data/tpvita/GZ2E01.iso", size, DiscCheck::InvalidImage, "L");
  const std::string st = join(shortScreen);
  CHECK(contains(st, "cannot be used"));
  CHECK(contains(st, "ux0:data/tpvita/GZ2E01.iso"));
  CHECK(contains(st, std::to_string(size) + " bytes (600.0 MiB)"));
  CHECK(contains(st, std::to_string(kGameCubeDiscBytes - size) + " bytes shorter"));
  CHECK(contains(st, "not a GameCube disc image that this build can read"));
  // full-size but still rejected: no "shorter" claim, the container hint instead
  const Screen full = bad_disc_screen("a/b.iso", kGameCubeDiscBytes, DiscCheck::InvalidImage, "L");
  CHECK(!contains(join(full), "shorter") && contains(join(full), "Wii images"));
  // a CISO is compressed: never called short
  CHECK(!contains(join(bad_disc_screen("a/b.ciso", 1000, DiscCheck::InvalidImage, "L")), "shorter"));
  // size 0 (unknown) is not "short"
  CHECK(!contains(join(bad_disc_screen("a/b.iso", 0, DiscCheck::IoError, "L")), "shorter"));
  CHECK(contains(join(bad_disc_screen("a/b.iso", 0, DiscCheck::IoError, "L")), "could not be opened"));
  for (DiscCheck c : {DiscCheck::Ok, DiscCheck::IoError, DiscCheck::InvalidImage, DiscCheck::WrongGame,
                      DiscCheck::WrongVersion, DiscCheck::Other})
    CHECK(std::string(disc_check_text(c)).size() > 10);

  // Both screens fit the real display: 960x544 at scale 2 -> 80 columns x 29 rows.
  const int scale = 2, margin = 16;
  const int cols = text_cols(960, scale, margin), rows = text_rows(544, scale, margin);
  CHECK(cols == 77 && rows == 28);
  for (const Screen* s : {&none, &saved, &shortScreen, &full}) {
    const auto laid = layout(*s, cols, rows);
    CHECK(static_cast<int>(laid.size()) == rows);
    for (const auto& l : laid) CHECK(static_cast<int>(l.text.size()) <= cols);
    CHECK(laid.back().text == kExitFooter);
    CHECK(!contains(join(laid), "more lines"));  // nothing is cut in these cases
  }

  if (const char* dir = std::getenv("TPV_SCREEN_DIR")) {
    std::vector<uint32_t> px(960u * 544u);
    const std::pair<const char*, const Screen*> shots[] = {
        {"no_disc", &none}, {"saved_disc", &saved}, {"bad_disc_short", &shortScreen}, {"bad_disc_full", &full}};
    for (const auto& [name, s] : shots) {
      Canvas c{px.data(), 960, 544, 960};
      render(c, layout(*s, cols, rows), scale, margin);
      const std::string path = std::string(dir) + "/" + name + ".ppm";
      FILE* f = std::fopen(path.c_str(), "wb");
      std::fprintf(f, "P6\n960 544\n255\n");
      for (uint32_t p : px) {
        const unsigned char rgb3[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                       static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb3, 1, 3, f);
      }
      std::fclose(f);
    }
  }
}

// ---- config lookup and boot log ------------------------------------------------------------------------

static void test_decide() {
  using namespace tpv;
  DiscInputs base;
  base.bootLogPath = "ux0:data/tpvita/boot.log";
  base.scans = {RootScan{"ux0:data/tpvita/", true, {}}};
  const std::string kFound = "ux0:data/tpvita/GZ2E01.iso", kSaved = "uma0:games/tp.iso";
  auto text = [](const DiscDecision& d) { return join(d.screen); };

  // 1. a disc in the port's folder that the game accepts is injected, whatever config.json says
  DiscInputs in = base;
  in.found = kFound;
  DiscDecision d = decide_disc(in);
  CHECK(d.ok && d.inject == kFound);
  in.saved = kSaved; in.savedExists = true; in.savedCheck = DiscCheck::InvalidImage;
  d = decide_disc(in);
  CHECK(d.ok && d.inject == kFound);

  // 2. nothing found, nothing saved: the "no disc" screen, no mention of config.json
  d = decide_disc(base);
  CHECK(!d.ok && d.inject.empty());
  CHECK(contains(text(d), "no disc image found") && !contains(text(d), "config.json"));

  // 3. found but rejected, nothing saved: say what is wrong with that file
  in = base; in.found = kFound; in.foundCheck = DiscCheck::InvalidImage; in.foundSize = 100;
  d = decide_disc(in);
  CHECK(!d.ok && contains(text(d), "cannot be used") && contains(text(d), kFound));
  CHECK(contains(text(d), "not a GameCube disc image that this build can read"));

  // 4. found but rejected, saved path fine: the game uses the saved path, nothing is injected
  in.saved = kSaved; in.savedExists = true; in.savedCheck = DiscCheck::Ok;
  d = decide_disc(in);
  CHECK(d.ok && d.inject.empty());

  // 5. found but rejected, saved also rejected: the disc the user just copied is the one named
  in.savedCheck = DiscCheck::WrongGame;
  d = decide_disc(in);
  CHECK(!d.ok && contains(text(d), kFound) && !contains(text(d), kSaved));

  // 6. nothing found, saved path fine
  in = base; in.saved = kSaved; in.savedExists = true; in.savedCheck = DiscCheck::Ok;
  d = decide_disc(in);
  CHECK(d.ok && d.inject.empty());

  // 7. nothing found, saved path rejected: name that file and the reason
  in.savedCheck = DiscCheck::WrongGame; in.savedSize = 1459978240;
  d = decide_disc(in);
  CHECK(!d.ok && contains(text(d), "cannot be used") && contains(text(d), kSaved));
  CHECK(contains(text(d), "not a Twilight Princess version the game accepts"));

  // 8. nothing found, saved path names a file that is gone: "no disc", plus the saved path and why
  in = base; in.saved = kSaved; in.savedExists = false;
  d = decide_disc(in);
  CHECK(!d.ok && contains(text(d), "no disc image found") && contains(text(d), kSaved) &&
        contains(text(d), "does not exist"));

  // 9. a saved path whose file is gone never counts as usable, even with a stale "Ok" verdict
  in.savedCheck = DiscCheck::Ok;
  CHECK(!decide_disc(in).ok);

  // 10. every outcome that is not ok carries a screen with the exit hint; ok outcomes carry none
  for (bool foundOk : {false, true})
    for (bool haveFound : {false, true})
      for (bool haveSaved : {false, true})
        for (bool savedExists : {false, true})
          for (bool savedOk : {false, true}) {
            DiscInputs c = base;
            if (haveFound) { c.found = kFound; c.foundCheck = foundOk ? DiscCheck::Ok : DiscCheck::IoError; }
            if (haveSaved) { c.saved = kSaved; c.savedExists = savedExists; c.savedCheck = savedOk ? DiscCheck::Ok : DiscCheck::Other; }
            const DiscDecision r = decide_disc(c);
            const bool expectOk = (haveFound && foundOk) || (haveSaved && savedExists && savedOk);
            CHECK(r.ok == expectOk);
            CHECK(r.ok == r.screen.title.empty());
            if (!r.ok) CHECK(r.screen.footer == kExitFooter);
            CHECK(r.inject == ((haveFound && foundOk) ? kFound : std::string()));
          }
}

static void test_json() {
  using namespace tpv;
  std::string v;
  CHECK(extract_json_string(R"({"backend.isoPath": "ux0:data/tpvita/GZ2E01.iso"})", "backend.isoPath", v));
  CHECK(v == "ux0:data/tpvita/GZ2E01.iso");
  CHECK(extract_json_string("{\n  \"a\" : 1,\n  \"backend.isoPath\"\n :\n \"x y\"\n}", "backend.isoPath", v) && v == "x y");
  CHECK(extract_json_string(R"({"backend.isoPath":"C:\\games\\tp \"best\".iso"})", "backend.isoPath", v));
  CHECK(v == "C:\\games\\tp \"best\".iso");
  CHECK(extract_json_string(R"({"backend.isoPath":"caf\u00e9/\/x\n"})", "backend.isoPath", v));
  CHECK(v == "caf\xC3\xA9//x\n");
  CHECK(extract_json_string(R"({"backend.isoPath":""})", "backend.isoPath", v) && v.empty());
  // the key text appearing as a VALUE earlier must not be taken for the key
  CHECK(extract_json_string(R"({"note": "backend.isoPath", "backend.isoPath": "real"})", "backend.isoPath", v));
  CHECK(v == "real");
  // a prefix of the key is not the key
  CHECK(!extract_json_string(R"({"backend.isoPathX": "no"})", "backend.isoPath", v));
  // absent, null, number, unterminated, truncated escape, bad hex
  CHECK(!extract_json_string("{}", "backend.isoPath", v));
  CHECK(!extract_json_string("", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": null})", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": 5})", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": "abc)", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": "abc\)", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": "\u12)", "backend.isoPath", v));
  CHECK(!extract_json_string(R"({"backend.isoPath": "\u12zz"})", "backend.isoPath", v));
  // never crashes on arbitrary bytes (ASan/UBSan are on)
  std::mt19937 rng(3);
  const std::string base = R"({"backend.isoPath": "a\u00e9\"b"})";
  for (int i = 0; i < 4000; ++i) {
    std::string t = base;
    for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) {
      const size_t at = rng() % t.size();
      switch (rng() % 3) {
        case 0: t[at] = static_cast<char>(rng()); break;
        case 1: t.erase(at, 1 + rng() % 3); break;
        default: t.insert(at, 1, static_cast<char>(rng())); break;
      }
      if (t.empty()) t = "x";
    }
    extract_json_string(t, "backend.isoPath", v);
  }
  CHECK(true);
}

static void test_bootlog(const std::string& tmp) {
  using namespace tpv;
  const std::string dir = tmp + "/log/tpvita";
  CHECK(make_dir(tmp + "/log"));
  CHECK(make_dir(dir));
  CHECK(make_dir(dir));  // already there is fine
  CHECK(!make_dir(tmp + "/nope/deeper/still"));
  std::string text;
  {
    BootLog log;
    log.echo = false;
    CHECK(!log.is_open());
    CHECK(log.open(dir));
    CHECK(log.is_open() && log.path() == dir + "/boot.log");
    log.line("first %d %s", 1, "run");
    // flushed per line: readable while the log is still open (a crash would not lose it)
    CHECK(read_text_file(dir + "/boot.log", text) && contains(text, "first 1 run\n"));
    CHECK(text[0] == '[' && text.find("] first") != std::string::npos);
    log.line("second");
    CHECK(read_text_file(dir + "/boot.log", text) && contains(text, "second\n"));
  }
  {
    BootLog log;  // the next start rotates the previous log
    log.echo = false;
    CHECK(log.open(dir));
    log.line("other run");
    log.close();
    CHECK(read_text_file(dir + "/boot.log", text) && contains(text, "other run") && !contains(text, "first 1"));
    CHECK(read_text_file(dir + "/boot.log.1", text) && contains(text, "first 1 run") && contains(text, "second"));
  }
  {
    BootLog log;  // a third start replaces .1, it does not accumulate
    log.echo = false;
    CHECK(log.open(dir));
    log.line("third");
    log.close();
    CHECK(read_text_file(dir + "/boot.log.1", text) && contains(text, "other run") && !contains(text, "first 1"));
  }
  // an unwritable folder: open fails, line() is still safe
  BootLog bad;
  bad.echo = false;
  CHECK(!bad.open(tmp + "/does/not/exist"));
  bad.line("goes nowhere, must not crash %d", 5);
  // a very long line is truncated, not overflowed
  BootLog big;
  big.echo = false;
  CHECK(big.open(dir));
  big.line("%s", std::string(5000, 'x').c_str());
  big.close();
  CHECK(read_text_file(dir + "/boot.log", text) && text.size() < 1200);
}

static void test_shader_dump(const std::string& tmp) {
  using aurora::gfx::vita::dump_shader_source;
  const std::string dir = tmp + "/shader_fail";  // does not exist yet: the helper creates it
  const std::string src = "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0); }\n";
  const std::string path = dump_shader_source(dir, "gx-1a2b3c4d", "frag", src);
  CHECK(path == dir + "/gx-1a2b3c4d.frag.glsl");
  std::string back;
  CHECK(tpv::read_text_file(path, back) && back == src);  // exactly what the compiler was given
  const std::string vpath = dump_shader_source(dir, "gx-1a2b3c4d", "vert", "v");
  CHECK(vpath == dir + "/gx-1a2b3c4d.vert.glsl" && vpath != path);
  // a label cannot climb out of the directory or smuggle in a separator
  const std::string evil = dump_shader_source(dir, "../../etc/passwd x", "frag", "e");
  CHECK(evil == dir + "/.._.._etc_passwd_x.frag.glsl");
  CHECK(tpv::read_text_file(evil, back) && back == "e");
  CHECK(dump_shader_source(dir, nullptr, nullptr, "n") == dir + "/shader.src.glsl");
  // a larger source (real programs are 10-20 KB) and bytes that are not text survive intact
  std::string big(40000, 'x');
  big[100] = '\0';
  big[200] = '\xFF';
  const std::string bigPath = dump_shader_source(dir, "big", "vert", big);
  CHECK(!bigPath.empty() && tpv::read_text_file(bigPath, back, 1 << 20) && back == big);
  // nothing to write to
  CHECK(dump_shader_source("", "x", "frag", "s").empty());
  CHECK(dump_shader_source(tmp + "/missing/parent/dir", "x", "frag", "s").empty());
}

int main() {
  char tmpl[] = "/tmp/tpv_diag_XXXXXX";
  const std::string tmp = mkdtemp(tmpl);
  test_font();
  test_draw();
  test_wrap();
  test_layout();
  test_scan(tmp);
  test_screens(tmp);
  test_decide();
  test_json();
  test_bootlog(tmp);
  test_shader_dump(tmp);
  std::string cmd = "chmod -R u+rwx " + tmp + " && rm -rf " + tmp;
  if (std::system(cmd.c_str()) != 0) std::printf("cleanup failed\n");
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail != 0;
}
