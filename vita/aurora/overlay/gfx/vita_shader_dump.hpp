// Saves the source of a shader that the GL implementation refused, so that a failure on a real Vita leaves
// the exact text behind. vitaGL translates GLSL ES 1.00 through Sony's Cg compiler, which is stricter than
// Mesa: the translator's error message alone is rarely enough to see what it objected to.
//
// Header-only (C++ and POSIX only); tests/host/disc/test_diag.cpp checks it.
#pragma once

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <string>

namespace aurora::gfx::vita {

// Writes `source` to `<dir>/<label>.<stage>.glsl` and returns that path, or "" when the file could not be
// written. `label` is made safe for a file name; `dir` is created if its parent exists. The file holds the
// source exactly as it was handed to the compiler.
inline std::string dump_shader_source(const std::string& dir, const char* label, const char* stage,
                                      const std::string& source) {
  if (dir.empty()) {
    return {};
  }
  ::mkdir(dir.c_str(), 0777); // fails harmlessly when it exists
  std::string name = label != nullptr ? label : "shader";
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
      c = '_';
    }
  }
  const std::string path = dir + "/" + name + "." + (stage != nullptr ? stage : "src") + ".glsl";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return {};
  }
  const bool ok = std::fwrite(source.data(), 1, source.size(), f) == source.size();
  std::fclose(f);
  return ok ? path : std::string();
}

} // namespace aurora::gfx::vita
