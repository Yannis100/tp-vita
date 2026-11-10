# String patches applied on top of upstream aurora/lib (see tools/build_aurora_tree.py).
# Every `old` must match exactly once.
#
# Target: encounter/aurora @ 3227d76 ("Update RmlUi").
#
# IMPORTANT: this is the version Dusklight actually pins. `git ls-tree HEAD extern/aurora` in
# dusklight 77705f8 returns 3227d76, and the newer aurora (505b5eb, 7 commits on) is not usable
# yet: it deletes 22 public PAD functions (PADButtonMapping, PADAxisMapping, PADDeadZones,
# PADDefaultMapping, PADGet/SetButtonMappings, PADGet/SetAxisMappings, the PAD key-binding
# accessors, PADSerializeMappings, PADSetKeyboardActive, ...) in favour of a new
# aurora::input binding system. Dusklight's own sources still compile against the old API, so
# moving the submodule forward breaks dusk/ui/controller_config.cpp, dusk/ui/input.cpp,
# dusk/menu_pointer.cpp, dusk/ui/overlay.cpp, dusk/ui/string_button.cpp, dusk/ui/command_console.cpp,
# m_Do/m_Do_main.cpp and d/d_drawlist.cpp (GXPeekZ). Do not bump until Dusklight migrates.
#
# The patches below are proven: tools/build_aurora_tree.py output for this file is byte-identical to
# .snapshot-pre-aurora-upgrade-20261029/aurora_lib-merged.tar.gz, which is the tree the known-good
# VPK was built from.
PATCHES = [
    # gx.hpp: drop the three WebGPU declarations; the Vita renderer has its own shader/pipeline path.
    ("gx/gx.hpp", "extern wgpu::BindGroup g_emptyTextureBindGroup;\n", ""),
    ("gx/gx.hpp",
     """wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout,
                                    const PipelineOptions& options, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept;
std::string build_shader_source(const ShaderConfig& config, DstAlphaMode dstAlphaMode,
                                uint32_t normalAttachment = UINT32_MAX) noexcept;
wgpu::ShaderModule build_shader(const ShaderConfig& config, const gfx::RenderTargetLayout& layout,
                                DstAlphaMode dstAlphaMode) noexcept;
""", ""),
]
PATCHES += [
    # The WebGPU device header must not be pulled into the GX frontend on Vita.
    ("dolphin/gx/GXFrameBuffer.cpp", '#include "../../webgpu/gpu.hpp"\n', ""),
]
PATCHES += [
    # GL depth range is [0,1] with the standard (not reversed) mapping; the vertex shader converts
    # Aurora's WebGPU-style z' = z + w (see fill_uniform) to GL clip space.
    ("gx/gx.hpp", "constexpr bool UseReversedZ = true;", "constexpr bool UseReversedZ = false;"),
]
PATCHES += [
    # The FIFO command processor must run on the calling thread (Drain), not a worker thread.
    # vitaGL is not thread-safe: a Thread-mode worker would replay draws from a second thread and
    # race the GL context state (bound FBO, program, texture units) against begin_frame/end_frame.
    ("gx/fifo.cpp",
     "constexpr auto kProcessingMode = ProcessingMode::Thread;",
     "constexpr auto kProcessingMode = ProcessingMode::Drain;"),
]
PATCHES += [
    # DVD worker: make it single-threaded. The worker thread itself is fine now that the pthread
    # symbols are forced in at link time (see tp-vita/CMakeLists.txt), but this port still reads the
    # disc synchronously: enqueue() executes the command inline instead of handing it to a worker,
    # so DVDReadPrio() and friends never block on a thread that is not there.
    #
    # start(): do not spawn the worker.
    ("dolphin/dvd/dvd.cpp",
     """    m_shutdown = false;
    m_thread = std::thread([this] { run(); });
    m_running = true;""",
     """    m_shutdown = false;
    // No worker thread on Vita: enqueue() runs the command on the calling thread (see below).
    m_running = true;"""),
    # stop(): there is no worker to detach from or join.
    ("dolphin/dvd/dvd.cpp",
     """      if (std::this_thread::get_id() == m_thread.get_id()) {
        m_running = false;
        m_thread.detach();
        stoppedFromWorker = true;
      } else if (m_activeBlock != nullptr) {
        m_cancelActiveBlock = m_activeBlock;
      }""",
     """      if (m_activeBlock != nullptr) {
        m_cancelActiveBlock = m_activeBlock;
      }"""),
    ("dolphin/dvd/dvd.cpp",
     """    m_cv.notify_all();
    m_thread.join();
    {
      std::lock_guard lk{m_mutex};
      m_running = false;
    }""",
     """    m_cv.notify_all();
    // No worker thread to join on Vita.
    {
      std::lock_guard lk{m_mutex};
      m_running = false;
    }"""),
    # enqueue(): run the command now instead of waking a worker.
    ("dolphin/dvd/dvd.cpp",
     """    if (executeNow) {
      execute(block);
      return;
    }
    m_cv.notify_one();
  }""",
     """    if (executeNow) {
      execute(block);
      return;
    }
    // Single-threaded build: never waits, so process the command now while it is still queued.
    {
      std::lock_guard lk(m_mutex);
      for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (*it == block) {
          m_queue.erase(it);
          break;
        }
      }
      m_activeBlock = block;
    }
    atomic_store_release(block->state, DVD_STATE_BUSY);
    execute(block);
    {
      std::lock_guard lk(m_mutex);
      m_activeBlock = nullptr;
    }
    m_doneCv.notify_all();
  }"""),
    # retire_command() / wait(): no worker means the "am I the worker?" guard can never be true.
    ("dolphin/dvd/dvd.cpp",
     """    if (m_activeBlock == block && std::this_thread::get_id() != m_thread.get_id()) {
      m_cancelActiveBlock = block;
      m_doneCv.wait(lk, [&] { return !m_running || m_activeBlock != block; });
    } else {""",
     """    if (m_activeBlock == block) {
      m_cancelActiveBlock = block;
      m_doneCv.wait(lk, [&] { return !m_running || m_activeBlock != block; });
    } else {"""),
    ("dolphin/dvd/dvd.cpp",
     """    if (m_activeBlock == block && std::this_thread::get_id() != m_thread.get_id()) {
      m_cancelActiveBlock = block;
      m_doneCv.wait(lk, [&] { return !m_running || m_activeBlock != block; });
    }""",
     """    if (m_activeBlock == block) {
      m_cancelActiveBlock = block;
      m_doneCv.wait(lk, [&] { return !m_running || m_activeBlock != block; });
    }"""),
]
PATCHES += [
    # std::call_once is replaced by a plain bool guard.
    #
    # Threading genuinely works on this toolchain now (CMakeLists.txt forces the four weak-undefined
    # pthread symbols in, so __gthread_active_p() is true and std::thread/std::mutex are real), but
    # AuroraInitClock() is reached from OSInit() on the main thread only, so the extra once_flag
    # machinery buys nothing here and this keeps the clock path free of libstdc++ once-init state
    # during early boot. If this ever needs to be reverted, delete these two patches; the symptom they
    # were originally added for was an abort inside OSInit().
    ("dolphin/os/OSTime.cpp",
     "std::once_flag s_gameEpochOnce;",
     "bool s_gameEpochInitialized = false;"),
    ("dolphin/os/OSTime.cpp",
     """void initialize_game_epoch() {
  s_gameEpochOffset""",
     """void initialize_game_epoch() {
  if (s_gameEpochInitialized) {
    return;
  }
  s_gameEpochInitialized = true;
  s_gameEpochOffset"""),
    ("dolphin/os/OSTime.cpp",
     "  std::call_once(s_gameEpochOnce, initialize_game_epoch);\n  return s_gameEpochOffset",
     "  initialize_game_epoch();\n  return s_gameEpochOffset"),
    ("dolphin/os/OSTime.cpp",
     "  std::call_once(s_gameEpochOnce, initialize_game_epoch);\n  if (OSBaseAddress == 0) {",
     "  initialize_game_epoch();\n  if (OSBaseAddress == 0) {"),
]

PATCHES += [
    # create_directories() must create PARENTS. SDL_CreateDirectory is documented as doing so, but
    # that is a property of each platform implementation, not of the API: the Vita backend is a bare
    # mkdir(2) (SDL_SYS_CreateDirectory in SDL_sysfsops.c.obj, one sceIoMkdir call and no loop, unlike
    # SDL_SYS_GetPrefPath in SDL_sysfilesystem.c.obj which does walk components). So on Vita
    # "ux0:data/App/EUR/Card A" fails with ENOENT while "EUR" is missing.
    #
    # That single behaviour is why the GameCube memory card never came up on this port: CARDInit's
    # "format the folder when it is absent" branch calls this with a two-level path whose parent (the
    # region directory) has never been created, so no card folder ever appeared, CardGciFolder::open()
    # left m_error = NOCARD, and every CARDCheck() reported NOCARD.
    #
    # The device root is skipped deliberately: "ux0:" is not a creatable directory and the Vita
    # platform layer cannot even stat it ("Failed to translate path: ux0:"), so asking for it would
    # turn the call into a failure on its first iteration.
    ("io.cpp",
     """    const auto pathString = fs_path_to_string(path);
    return SDL_CreateDirectory(pathString.c_str());""",
     """    // Create each prefix bottom-up instead of trusting SDL_CreateDirectory's documented parent
    // creation: the Vita backend is a bare mkdir and does not implement it (see PATCHES note).
    // Component-wise iteration (not string splitting) keeps "ux0:" and "."/".." handling correct.
    std::filesystem::path prefix;
    for (const auto& component : path) {
      prefix /= component;
      const auto prefixString = fs_path_to_string(prefix);
      if (prefixString.empty() || prefixString.back() == ':') {
        continue; // device root: not creatable, and not stat-able on Vita
      }
      // SDL_CreateDirectory succeeds when the leaf already exists as a directory, so no separate
      // exists() probe (a TOCTOU race, and an extra device round-trip on hardware).
      if (!SDL_CreateDirectory(prefixString.c_str())) {
        return false;
      }
    }
    return true;"""),
]
