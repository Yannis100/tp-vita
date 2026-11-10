/**
 * Vita port: Aurora backend.
 *
 * Replaces Aurora's WebGPU/Dawn backend (extern/aurora/lib/aurora.cpp, webgpu/, gfx/frame.cpp,
 * window.cpp). Aurora's GX frontend (FIFO, register decode, command processor) is kept; the draws it
 * records are replayed with vitaGL by gfx/vita_gl_replay.cpp at the end of every frame.
 *
 * Frame lifecycle (mirrors upstream aurora.cpp):
 *   begin_frame: fifo begin, record the EFB clear + initial viewport/scissor, ImGui NewFrame
 *   end_frame:   fifo drain/end, texture end, replay -> EFB, present EFB -> screen, ImGui, swap
 *
 * This file is compiled into tpv_aurora so it sees the merged Aurora tree (build/aurora_lib).
 */

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/time.hpp>
#include <dolphin/gx/GXAurora.h>

#include "gfx/recording.hpp"
#include "gfx/vita_frame.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "input.hpp"
#include "internal.hpp"
#include "logging.hpp"

#include "imgui_vita.hpp"

// Frame-counted input injection for automated runs (off unless ux0:data/tpvita/input_script.txt
// exists). Deliberately a port-owned file: it must not touch dusklight, so it lives in vita/src.
#include "../src/input_script.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#endif

namespace aurora {
namespace {
Module Log("aurora::vita");

constexpr uint32_t ScreenWidth = 960;
constexpr uint32_t ScreenHeight = 544;

// Dusklight asks for a 256 MiB MEM1 (a PC convenience). The GameCube had 24 MiB; the Vita has
// 365 MiB of MAIN in total, shared with the newlib heap, vitaGL's pools and thread stacks.
constexpr uint32_t VitaMem1Size = 64 * 1024 * 1024;
constexpr uint32_t VitaMem2Size = 24 * 1024 * 1024;
// RAM left for the system when vitaGL carves its RAM pool out of the free MAIN memory (see
// vita/src/main.cpp for the memory budget).
constexpr int VitaRamThreshold = 24 * 1024 * 1024;

const char* VitaUserPath = "ux0:data/tpvita/";

// The strings in AuroraConfig are owned by the *caller* only until aurora_initialize returns.
// Upstream (extern/aurora/lib/aurora.cpp) copies all four with strdup(); this backend did not, and
// that was the memory-card blocker (HANDOVER §3d): dusklight builds them from std::string
// temporaries in game_main(), the block that owns them ends, and the freed buffers are reused.
// Measured consequence: g_config.userPath ended up holding "ux0:data/tpvita/GZ2P01.iso" (the DISC
// image path, 28 bytes -- exactly the size of the freed ConfigPath buffer it was allocated into),
// so CARDInit resolved the card folder as "ux0:data/tpvita/GZ2P01.iso/EUR/Card A". Creating a
// directory under a regular file fails with ENOENT, CardGciFolder::open() left m_error = NOCARD,
// CARDCheck() reported NOCARD, and dFile_select_c::MemCardStatCheck looped on the "No Memory Card
// found in Slot A" prompt forever. Copy the strings so the lifetime matches upstream.
constexpr size_t kPathCopyMax = 512;

char* copy_string(const char* source, char* storage) {
  if (source == nullptr) {
    return nullptr;
  }
  const size_t length = std::strlen(source);
  if (length + 1 > kPathCopyMax) {
    // Truncating a path silently would produce a *different* directory (and a card that appears
    // to vanish). Refuse instead, and say so: every path here is short.
    Log.warn("path is {} bytes, over the {} byte copy limit: refusing", length, kPathCopyMax);
    return nullptr;
  }
  std::memcpy(storage, source, length + 1);
  return storage;
}

AuroraInfo sInfo;
AuroraStats sStats;
float sFps = 0.f;
bool sInitialized = false;
std::chrono::steady_clock::time_point sLastPresent;

AuroraWindowSize screen_size() {
  AuroraWindowSize size{};
  size.width = ScreenWidth;
  size.height = ScreenHeight;
  size.native_fb_width = ScreenWidth;
  size.native_fb_height = ScreenHeight;
  const auto fb = gfx::vita::main_target_size();
  size.fb_width = fb.x != 0 ? fb.x : 640;
  size.fb_height = fb.y != 0 ? fb.y : 480;
  size.scale = 1.f;
  return size;
}
} // namespace

AuroraInfo initialize(int, char**, const AuroraConfig& config) {
  g_config = config;
  // Own copies, so the caller's temporaries may die (see copy_string above). Static storage: these
  // live for the life of the process, which is what upstream's strdup achieves with a leak.
  static char sUserPath[kPathCopyMax];
  static char sCachePath[kPathCopyMax];
  static char sResourcesPath[kPathCopyMax];
  static char sAppName[kPathCopyMax];
  g_config.userPath = copy_string(g_config.userPath, sUserPath);
  g_config.cachePath = copy_string(g_config.cachePath, sCachePath);
  g_config.resourcesPath = copy_string(g_config.resourcesPath, sResourcesPath);
  g_config.appName = copy_string(g_config.appName, sAppName);
  if (g_config.userPath == nullptr) {
    g_config.userPath = VitaUserPath;
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = VitaUserPath;
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = "app0:/";
  }
  if (g_config.appName == nullptr) {
    g_config.appName = "tpvita";
  }
  // Logged because this value decides where the memory card, the shader cache and the config live.
  // When it is wrong the symptom is nowhere near the cause (the card prompt loop of HANDOVER §3d),
  // so make the resolved value visible on every boot.
  Log.info("paths: user '{}', cache '{}', resources '{}', app '{}'", g_config.userPath,
           g_config.cachePath, g_config.resourcesPath, g_config.appName);
  g_config.msaa = 1;
  if (g_config.mem1Size > VitaMem1Size) {
    Log.info("clamping MEM1 {} MiB -> {} MiB", g_config.mem1Size >> 20, VitaMem1Size >> 20);
    g_config.mem1Size = VitaMem1Size;
  }
  if (g_config.mem2Size > VitaMem2Size) {
    g_config.mem2Size = VitaMem2Size;
  }
  Log.info("Aurora (vitaGL) initializing");

#ifdef __vita__
  // All of these must be called before vglInit*.
  vglSetupRenderTargetScenesNum(2, 16); // display scenes, FBO scenes per frame (EFB copies/offscreens)
  vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, GL_FALSE, GL_FALSE, GL_FALSE);
  vglSetShaderCachePath("ux0:data/tpvita/shader_cache");
  // The source of a shader that vitaGL's Cg translator refuses is saved here (the log names the files).
  gfx::vita::set_shader_dump_dir("ux0:data/tpvita/shader_fail");
  Log.info("vitaGL: initialising {}x{}, {} MiB of RAM left for the system", ScreenWidth, ScreenHeight,
           VitaRamThreshold >> 20);
  vglInitWithCustomThreshold(0, ScreenWidth, ScreenHeight, VitaRamThreshold, 0, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
  vglWaitVblankStart(GL_TRUE);
  Log.info("vitaGL: initialised");
#endif
  gfx::vita::set_present_size(ScreenWidth, ScreenHeight);

  // Initialize SDL gamepad/joystick subsystems and enumerate connected controllers.
  // SDL3's Vita backend maps the physical Vita controls (SceCtrl/SceHid/SceTouch) to a
  // virtual "Vita controller" gamepad; add_controller() is normally driven by
  // SDL_EVENT_GAMEPAD_ADDED from the event pump (see aurora_update below).
  input::initialize();

  // Drain mode (not Thread) is forced by a patches.py string patch on gx/fifo.cpp: vitaGL owns
  // per-context GPU state (bound FBO, program, texture units) with no locking, so the command
  // processor must replay draws on this thread, and doing it once per drained batch rather than
  // after every command keeps the per-draw cost reasonable on a Cortex-A9.
  gx::fifo::init();
  vita::imgui::create_context(nullptr);
  const AuroraWindowSize size = screen_size();
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }

  sInfo = {};
  sInfo.backend = BACKEND_OPENGLES;
  sInfo.userPath = g_config.userPath;
  sInfo.cachePath = g_config.cachePath;
  sInfo.window = nullptr;
  sInfo.windowSize = size;
  sInitialized = true;
  sLastPresent = std::chrono::steady_clock::now();
  g_config.desiredBackend = BACKEND_OPENGLES;
  return sInfo;
}

void shutdown() {
  if (!sInitialized) {
    return;
  }
  input::shutdown();
  gx::fifo::shutdown();
  vita::imgui::shutdown();
  gfx::vita::shutdown_gl();
  sInitialized = false;
}

bool begin_frame() {
  gx::fifo::begin_frame();
  gfx::vita::begin_frame();
  vita::imgui::new_frame(ScreenWidth, ScreenHeight);
  return true;
}

void end_frame() {
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();

  const auto& recorded = gfx::vita::frame();
  sStats.drawCallCount = static_cast<uint32_t>(recorded.draws.size());
  sStats.lastVertSize = static_cast<uint32_t>(recorded.verts.size());
  sStats.lastUniformSize = static_cast<uint32_t>(recorded.uniforms.size());
  sStats.lastIndexSize = static_cast<uint32_t>(recorded.indices.size());
  sStats.lastStorageSize = static_cast<uint32_t>(recorded.storage.size());

  gfx::finish();
  gfx::vita::present();
  vita::imgui::render_and_draw(ScreenWidth, ScreenHeight);
#ifdef __vita__
  vglSwapBuffers(GL_FALSE);
#endif

  const auto now = std::chrono::steady_clock::now();
  const float dt = std::chrono::duration<float>(now - sLastPresent).count();
  sLastPresent = now;
  if (dt > 0.f) {
    sFps = sFps == 0.f ? 1.f / dt : sFps * 0.9f + (1.f / dt) * 0.1f;
  }
}

const AuroraStats* stats() { return &sStats; }
float fps() { return sFps; }
AuroraBackend backend() { return sInfo.backend; }
} // namespace aurora

extern "C" {
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }

const AuroraEvent* aurora_update() {
  // Pump the SDL event queue. On desktop this lives in Aurora's window.cpp; on Vita there is no
  // SDL window loop, so controller hotplug (SDL_EVENT_GAMEPAD_ADDED/REMOVED) is handled here so
  // aurora::input keeps g_GameControllers populated for PADRead. SDL3's Vita backend maps the
  // physical controls to a virtual "Vita controller" gamepad.
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      aurora::input::add_controller(event.gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      if (aurora::input::get_instance_for_player(0) == event.gdevice.which ||
          aurora::input::get_instance_for_player(1) == event.gdevice.which ||
          aurora::input::get_instance_for_player(2) == event.gdevice.which ||
          aurora::input::get_instance_for_player(3) == event.gdevice.which)
      {
        aurora::input::remove_controller(event.gdevice.which);
      }
      break;
    default:
      break;
    }
  }

  static AuroraEvent sNone;
  std::memset(&sNone, 0, sizeof(sNone));
  sNone.type = AURORA_NONE;
  // Advance the frame-counted input script, if one was supplied. This is the right hook because
  // aurora_update() runs once per main-loop iteration and BEFORE the game reads the pad, so the
  // injected state is already visible to this frame's PADRead. Doing it here also means dusklight
  // needs no edits at all: the script drives the real game through its real input path.
  tpv::input_script_frame();
  aurora::gx::update();
  return &sNone;
}

bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }

void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) { aurora::g_config.allowJoystickBackgroundEvents = value; }
void aurora_set_resampler(AuroraSampler) {}
void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }

AuroraBackend aurora_get_backend() { return aurora::backend(); }

const AuroraBackend* aurora_get_available_backends(size_t* count) {
  static const AuroraBackend backends[] = {BACKEND_OPENGLES};
  if (count != nullptr) {
    *count = 1;
  }
  return backends;
}

float aurora_get_timescale() { return aurora::time::scale(); }

// aurora/gfx.h
const AuroraStats* aurora_get_stats() { return aurora::stats(); }
float aurora_get_fps() { return aurora::fps(); }
void aurora_enable_vsync(bool enabled) {
#ifdef __vita__
  vglWaitVblankStart(enabled ? GL_TRUE : GL_FALSE);
#else
  (void)enabled;
#endif
}
} // extern "C"
