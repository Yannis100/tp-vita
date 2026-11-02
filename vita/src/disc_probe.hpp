// Locating the user's GameCube disc image. Header-only (POSIX + C++ only) so tests/host/disc can
// exercise it without the Vita SDK.
//
// Why this exists: the game only boots from `--dvd <path>` or a saved `backend.isoPath`; its fallback
// (the RmlUi pre-launch UI where the user picks the image) is stubbed on Vita, so without a path the
// first launch waits forever on a blank screen. main() therefore injects `--dvd` when it finds a disc.
//
// Only what vita/nod_lite can read is accepted: plain ISO/GCM and CISO (nod.h: RVZ/WIA/GCZ/WBFS/NFS
// return NOD_RESULT_ERR_FORMAT, and the game would fall back to the blank pre-launch loop again).
#pragma once

#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tpv {

inline bool has_disc_extension(const std::string& name) {
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == "iso" || ext == "gcm" || ext == "ciso";
}

// True when the file is a GameCube Twilight Princess disc, i.e. one the game's catalog accepts
// (dusk/iso_validate.cpp: GZ2E01 / GZ2J01 / GZ2P01; the Wii RZD* discs are not readable by nod_lite).
// The game ID is the first 6 bytes of a plain image; a CISO has a 0x8000-byte header and the disc
// starts right behind it.
inline bool is_twilight_princess_image(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  char head[4] = {};
  bool ok = std::fread(head, 1, 4, f) == 4;
  long discStart = 0;
  if (ok && std::memcmp(head, "CISO", 4) == 0) {
    discStart = 0x8000;
  }
  char id[6] = {};
  ok = ok && std::fseek(f, discStart, SEEK_SET) == 0 && std::fread(id, 1, 6, f) == 6;
  std::fclose(f);
  if (!ok) {
    return false;
  }
  for (const char* accepted : {"GZ2E01", "GZ2J01", "GZ2P01"}) {
    if (std::memcmp(id, accepted, 6) == 0) {
      return true;
    }
  }
  return false;
}

// Returns the first Twilight Princess image found, searching `roots` in order. Within a root the
// conventional name GZ2E01.* wins, then any *.iso / *.gcm / *.ciso (any case) in name order, so a
// disc named like a typical dump ("Legend of Zelda - Twilight Princess (USA).iso") is found too.
inline bool find_disc_image(const std::vector<std::string>& roots, std::string& out) {
  for (std::string root : roots) {
    while (root.size() > 1 && root.back() == '/') {
      root.pop_back();
    }
    for (const char* name : {"GZ2E01.iso", "GZ2E01.gcm", "GZ2E01.ciso"}) {
      const std::string candidate = root + "/" + name;
      if (is_twilight_princess_image(candidate)) {
        out = candidate;
        return true;
      }
    }
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) {
      continue;
    }
    std::vector<std::string> names;
    while (const dirent* entry = readdir(dir)) {
      if (has_disc_extension(entry->d_name)) {
        names.emplace_back(entry->d_name);
      }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const std::string candidate = root + "/" + name;
      if (is_twilight_princess_image(candidate)) {
        out = candidate;
        return true;
      }
    }
  }
  return false;
}

} // namespace tpv
