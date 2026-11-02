# Third-party libraries Dusklight/Aurora expect, built for arm-vita-eabi.
# Sources come from tools/fetch_deps.sh (pinned to Dusklight's versions).
# VitaSDK already provides: fmt 12.2, zstd, libpng, zlib, freetype, SDL3 3.4, vitaGL.

set(TPV_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Fetched third-party sources")
if(NOT EXISTS "${TPV_DEPS_DIR}/abseil/CMakeLists.txt")
  message(FATAL_ERROR "Missing deps: run tools/fetch_deps.sh first")
endif()

# --- Abseil (flat_hash_map, btree) ---
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${TPV_DEPS_DIR}/abseil ${CMAKE_BINARY_DIR}/_deps/abseil EXCLUDE_FROM_ALL)

# --- Header-only ---
add_library(tpv_tracy INTERFACE)          # profiler macros compile to nothing without TRACY_ENABLE
target_include_directories(tpv_tracy INTERFACE ${TPV_DEPS_DIR}/tracy/public)
add_library(Tracy::TracyClient ALIAS tpv_tracy)
add_library(TracyClient ALIAS tpv_tracy)

add_library(tpv_json INTERFACE)
target_include_directories(tpv_json INTERFACE ${TPV_DEPS_DIR}/json/single_include)
add_library(nlohmann_json::nlohmann_json ALIAS tpv_json)

add_library(tpv_cxxopts INTERFACE)
target_include_directories(tpv_cxxopts INTERFACE ${TPV_DEPS_DIR}/cxxopts/include)
add_library(cxxopts::cxxopts ALIAS tpv_cxxopts)

add_library(picosha2 INTERFACE)
target_include_directories(picosha2 INTERFACE ${TPV_DEPS_DIR}/picosha2)

# --- Small compiled libs ---
add_library(xxhash STATIC ${TPV_DEPS_DIR}/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${TPV_DEPS_DIR}/xxhash)
add_library(xxHash::xxhash ALIAS xxhash)

add_library(miniz STATIC ${TPV_DEPS_DIR}/miniz/miniz.c)
target_include_directories(miniz PUBLIC ${TPV_DEPS_DIR}/miniz)

# fmt / zstd / png from VitaSDK
add_library(tpv_fmt INTERFACE)
target_link_libraries(tpv_fmt INTERFACE fmt)
add_library(fmt::fmt ALIAS tpv_fmt)
add_library(tpv_zstd INTERFACE)
target_link_libraries(tpv_zstd INTERFACE zstd)
add_library(zstd::libzstd ALIAS tpv_zstd)
add_library(tpv_png INTERFACE)
target_link_libraries(tpv_png INTERFACE png16 z)
add_library(PNG::PNG ALIAS tpv_png)

# --- Dear ImGui (upstream 1.91.9b-docking; VitaSDK ships an ancient 1.61) ---
set(_imgui ${TPV_DEPS_DIR}/imgui)
add_library(imgui STATIC
  ${_imgui}/imgui.cpp ${_imgui}/imgui_draw.cpp ${_imgui}/imgui_tables.cpp
  ${_imgui}/imgui_widgets.cpp ${_imgui}/misc/cpp/imgui_stdlib.cpp
  ${TP_DEPS}/extern/aurora/lib/imgui_config.cpp)
# Upstream dir first so <imgui.h> does not resolve to VitaSDK's 1.61 copy.
target_include_directories(imgui BEFORE PUBLIC ${_imgui} ${_imgui}/misc/cpp ${TP_DEPS}/extern/aurora/include)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="aurora/imgui_config.h" IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# --- RmlUi (Dusklight's menus). Rendering goes through a Vita render interface. ---
block(SCOPE_FOR VARIABLES)
  set(BUILD_SHARED_LIBS OFF)
  set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
  set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
  set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
  set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
  add_subdirectory(${TPV_DEPS_DIR}/rmlui ${CMAKE_BINARY_DIR}/_deps/rmlui EXCLUDE_FROM_ALL)
endblock()
