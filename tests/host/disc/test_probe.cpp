// Host test for vita/src/disc_probe.hpp (disc image discovery).
//   tests/host/disc/run.sh
#include "disc_probe.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    ++g_checks;                                                  \
    if (!(cond)) {                                               \
      ++g_fail;                                                  \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);        \
    }                                                            \
  } while (0)

static void write_file(const std::string& path, const std::string& prefix, size_t at = 0) {
  FILE* f = std::fopen(path.c_str(), "wb");
  std::string data(at, '\0');
  data += prefix;
  data.resize(std::max<size_t>(data.size(), 0x9000), '\0');
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

int main() {
  char tmpl[] = "/tmp/tpv_disc_XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::string a = root + "/a/";   // trailing slash on purpose (as in the Vita roots)
  const std::string b = root + "/b";
  const std::string empty = root + "/empty";
  mkdir((root + "/a").c_str(), 0755);
  mkdir(b.c_str(), 0755);
  mkdir(empty.c_str(), 0755);
  std::string out;

  // nothing there / missing directory
  CHECK(!tpv::find_disc_image({empty}, out));
  CHECK(!tpv::find_disc_image({root + "/does-not-exist"}, out));

  // unsupported containers and other games are ignored, even with the conventional name
  write_file(root + "/a/GZ2E01.gcz", "GZ2E01");        // GCZ: nod_lite cannot read it
  write_file(root + "/a/GZ2E01.z64", "GZ2E01");        // not a GameCube extension
  write_file(root + "/a/mario-kart.iso", "GM4E01");    // wrong game
  write_file(root + "/a/short.iso", "GZ");             // truncated header is padded, but ID is "GZ\0..."
  write_file(root + "/a/gz2x.iso", "GZ2X01");          // right prefix, not a game ID the catalog accepts
  write_file(root + "/a/wii.iso", "RZDE01");           // Wii disc: nod_lite cannot read it
  CHECK(!tpv::find_disc_image({a}, out));

  // a oddly named plain image is found by scanning, any extension case
  write_file(root + "/a/Legend of Zelda - Twilight Princess (USA).ISO", "GZ2E01");
  CHECK(tpv::find_disc_image({a}, out));
  CHECK(out == root + "/a/Legend of Zelda - Twilight Princess (USA).ISO");

  // the conventional name wins over scanning
  write_file(root + "/a/GZ2E01.iso", "GZ2E01");
  CHECK(tpv::find_disc_image({a}, out));
  CHECK(out == root + "/a/GZ2E01.iso");

  // roots are searched in order; PAL/JP IDs (GZ2P01 / GZ2J01) are accepted too
  write_file(root + "/b/dump.gcm", "GZ2P01");
  CHECK(tpv::find_disc_image({empty, b}, out));
  CHECK(out == b + "/dump.gcm");
  CHECK(tpv::find_disc_image({a, b}, out));
  CHECK(out == root + "/a/GZ2E01.iso");

  // CISO: the game ID sits behind the 0x8000-byte header
  const std::string c = root + "/c";
  mkdir(c.c_str(), 0755);
  write_file(c + "/tp.ciso", "GZ2E01", 0x8000);
  {
    // CISO magic first, disc ID at 0x8000
    FILE* f = std::fopen((c + "/tp.ciso").c_str(), "r+b");
    std::fwrite("CISO", 1, 4, f);
    std::fclose(f);
  }
  CHECK(tpv::find_disc_image({c}, out));
  CHECK(out == c + "/tp.ciso");
  // a CISO of another game is rejected
  const std::string d = root + "/d";
  mkdir(d.c_str(), 0755);
  write_file(d + "/other.ciso", "GM4E01", 0x8000);
  {
    FILE* f = std::fopen((d + "/other.ciso").c_str(), "r+b");
    std::fwrite("CISO", 1, 4, f);
    std::fclose(f);
  }
  CHECK(!tpv::find_disc_image({d}, out));

  std::string cmd = "rm -rf " + root;
  if (std::system(cmd.c_str()) != 0) {
    std::printf("cleanup failed\n");
  }
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail != 0;
}
