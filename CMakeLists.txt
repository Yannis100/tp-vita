# Twilight Princess for PS Vita — Dusklight (TwilitRealm, CC0) game code on a Vita platform layer.
#
# Layout:
#   TP_DEPS          Dusklight checkout (branch `vita`): game code, JSystem, Aurora, Borealis.
#   deps/            third-party sources fetched by tools/fetch_deps.sh
#   vita/aurora/     Vita replacements for Aurora's WebGPU/SDL/nod backends (overlaid on Aurora's lib/)
#   vita/src/        Vita entry point, platform glue, stubs for desktop-only features
#
# Build:  tools/fetch_deps.sh && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(tp_vita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Twilight Princess")
set(VITA_TITLEID  "TPLG00001")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")   # 365 MiB main budget

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# --- Dusklight checkout -------------------------------------------------------
if(NOT DEFINED TP_DEPS)
  get_filename_component(TP_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../dusklight" ABSOLUTE)
endif()
if(NOT EXISTS "${TP_DEPS}/files.cmake")
  message(FATAL_ERROR "TP_DEPS must point at a Dusklight checkout (got ${TP_DEPS})")
endif()
message(STATUS "TP_DEPS=${TP_DEPS}")
set(AURORA_DIR   "${TP_DEPS}/extern/aurora")
set(BOREALIS_DIR "${TP_DEPS}/extern/borealis")
set(TPV_DIR      "${CMAKE_CURRENT_SOURCE_DIR}")

# Flags shared by everything we compile. -fsigned-char matches the GameCube (and x86).
# arm-vita-eabi defaults to -fshort-enums (AAPCS bare-metal); the game's structs and data
# layouts assume int-sized enums like every other Dusklight target.
add_compile_options(
  -fsigned-char -fno-short-enums -g
  $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-multichar>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-trigraphs>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-template-body>
  -Wno-psabi
)

add_link_options(-Wl,--no-enum-size-warning)

include(cmake/deps.cmake)
include(cmake/aurora.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(cmake/aurora_gx.cmake)
include(cmake/borealis.cmake)
include(cmake/game_sources.cmake)

# --- Game ---------------------------------------------------------------------
set(TPV_GAME_DEFS
  TARGET_PC=1 WIDESCREEN_SUPPORT=1 AVOID_UB=1 VERSION=0 MTX_USE_PS=1 PARTIAL_DEBUG=1
  DUSK_BUILDING_GAME=1 DUSK_HAS_FUNCHOOK=0 DUSK_HAS_PREPATCH=0 TARGET_VITA=1)
set(TPV_GAME_INCLUDES
  ${TPV_DIR}/vita/include                 # Vita overrides (searched first)
  ${TP_DEPS}/include
  ${TP_DEPS}/assets/GZ2E01
  ${TP_DEPS}/libs/JSystem/include
  ${AURORA_DIR}/include/dolphin
  ${AURORA_DIR}/include
  ${TP_DEPS}/sdk/include
  ${TP_DEPS}/src
  ${TP_DEPS}/extern
  ${CMAKE_CURRENT_BINARY_DIR})
set(TPV_GAME_LIBS tpv_aurora tpv_borealis absl::flat_hash_map absl::btree nlohmann_json::nlohmann_json
  Tracy::TracyClient fmt::fmt zstd::libzstd picosha2 PNG::PNG miniz cxxopts::cxxopts imgui rmlui xxHash::xxhash)

# JSystem (no PCH upstream).
add_library(tpv_jsystem OBJECT ${TPV_JSYSTEM_FILES})
target_compile_definitions(tpv_jsystem PRIVATE ${TPV_GAME_DEFS})
target_include_directories(tpv_jsystem PRIVATE ${TPV_GAME_INCLUDES})
target_link_libraries(tpv_jsystem PRIVATE ${TPV_GAME_LIBS})

# Game code (d_*, m_Do, f_*, actors, Z2Audio, SSystem) and Dusklight's PC layer, sharing the PCH.
set(TPV_DUSK_EXCLUDE
  # desktop-only: code mods via native libraries / prepatching, Discord, LiveSplit
  "src/dusk/mods/loader/code_patch_macos.cpp"
  "src/dusk/mods/loader/prepatch.cpp"
  # WebGPU/Dawn-bound mods service files - no WebGPU on Vita (mods feature off).
  # sdk/include/mods/svc/gfx.h is still parsed by the game (m_Do_graphic.cpp) via the
  # vita/include/webgpu/webgpu.h shim; only the implementations are excluded.
  "src/dusk/mods/svc/gfx.cpp"
  "src/dusk/mods/svc/camera.cpp"
  # Desktop-only RmlUI icon provider — pulls the Aurora WebGPU/Dawn C++ layer (webgpu/webgpu_cpp.h),
  # which has no Vita backend. A Vita-side stub (vita/src/dusk_ui_icon_provider_stub.cpp) provides
  # the same entry points so the callers (d_meter2_draw / ui / touch_controls) still link.
  "src/dusk/ui/icon_provider.cpp")
set(_dusk_files ${DUSK_FILES})
foreach(_x IN LISTS TPV_DUSK_EXCLUDE)
  list(REMOVE_ITEM _dusk_files "${TP_DEPS}/${_x}")
endforeach()

# Files upstream lists outside files.cmake (in CMakeLists.txt itself).
set(_extra_dusk_files
  ${TP_DEPS}/src/dusk/main.cpp
  ${TP_DEPS}/src/dusk/audio/DuskAudioSystem.cpp
  ${TP_DEPS}/src/dusk/audio/JASCriticalSection.cpp
  ${TP_DEPS}/src/dusk/audio/DuskDsp.cpp
  ${TP_DEPS}/src/dusk/audio/Adpcm.cpp
  ${TP_DEPS}/src/dusk/audio/DspStub.cpp
  ${TP_DEPS}/src/dusk/imgui/ImGuiAudio.cpp)
list(APPEND _dusk_files ${_extra_dusk_files})

add_library(freeverb STATIC ${TP_DEPS}/libs/freeverb/comb.cpp ${TP_DEPS}/libs/freeverb/allpass.cpp ${TP_DEPS}/libs/freeverb/revmodel.cpp)
target_include_directories(freeverb PRIVATE ${TP_DEPS}/libs/freeverb/include/freeverb)
target_include_directories(freeverb PUBLIC ${TP_DEPS}/libs/freeverb/include)
list(APPEND TPV_GAME_LIBS freeverb)

add_library(tpv_game OBJECT
  ${DOLZEL_FILES} ${Z2AUDIOLIB_FILES} ${REL_FILES} ${DOLPHIN_FILES} ${SSYSTEM_FILES} ${_dusk_files}
  ${TPV_DIR}/vita/src/dusk_ui_icon_provider_stub.cpp
  ${TPV_DIR}/vita/src/dusk_mods_stub.cpp)
target_compile_definitions(tpv_game PRIVATE ${TPV_GAME_DEFS})
target_include_directories(tpv_game PRIVATE ${TPV_GAME_INCLUDES})
target_link_libraries(tpv_game PRIVATE ${TPV_GAME_LIBS})
target_precompile_headers(tpv_game PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${TP_DEPS}/include/dusk_pch.hpp>")
set_source_files_properties(${DOLZEL_FILES} ${Z2AUDIOLIB_FILES} ${REL_FILES} ${DOLPHIN_FILES} ${_dusk_files}
  TARGET_DIRECTORY tpv_game PROPERTIES COMPILE_DEFINITIONS "NDEBUG=1;NDEBUG_DEFINED=1;DEBUG_DEFINED=0")

# --- Vita platform layer: aurora backend + entry point -----------------------
# Replaces Aurora's WebGPU/Dawn backend and the desktop boot glue. backend.cpp implements the
# Aurora C API on vitaGL (init, frame lifecycle, input pump); the GX frontend, vertex decoder,
# shader generator, texture module and GL replay come from the merged tree (cmake/aurora_gx.cmake).
# aurora_stubs.cpp still stubs window management, RmlUi and texture replacements.
set(TPV_VITA_SOURCES
  ${TPV_DIR}/vita/src/main.cpp
  ${TPV_DIR}/vita/src/boot_screen.cpp
  ${TPV_DIR}/vita/src/borealis_cli_stub.cpp
  ${TPV_DIR}/vita/src/libc_stubs.cpp
  ${TPV_DIR}/vita/src/text_pad.cpp
  # Crash reporting: --wrap reaches every abort(); crash_symbols.cpp resolves the address.
  ${TPV_DIR}/vita/src/crash_record.cpp
  ${TPV_DIR}/vita/src/crash_symbols.cpp
  ${TPV_DIR}/vita/aurora/backend.cpp
  ${TPV_DIR}/vita/aurora/aurora_stubs.cpp
  ${TPV_DIR}/vita/aurora/imgui_vita.cpp
)

# The game provides aurora_main (from dusk/main.cpp via the main.h rename).
# We link its own `main` entry point that forwards to aurora_main.
add_executable(tpv_vita ${TPV_VITA_SOURCES})
target_compile_definitions(tpv_vita PRIVATE ${TPV_GAME_DEFS})
# The merged Aurora tree (build/aurora_lib) must shadow the upstream extern/aurora/lib so the
# Vita overlay headers (gfx/types.hpp, gfx/recording.hpp, etc.) win over the WebGPU-bound ones.
target_include_directories(tpv_vita PRIVATE
  ${TPV_GAME_INCLUDES}
  ${CMAKE_CURRENT_BINARY_DIR}/aurora_lib
  ${TPV_DIR}/vita/aurora
  ${AURORA_DIR}/lib)
# SDL3 is linked transitively via tpv_aurora/TPV_GAME_LIBS (raw -lSDL3 on Vita).
# SDL3 3.4's Vita backend needs these system stub libraries (see $VITASDK .../pkgconfig/sdl3.pc).
set(TPV_VITA_SYS_LIBS
  SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub SceAppUtil_stub SceAudio_stub SceAudioIn_stub
  SceSysmodule_stub SceIofilemgr_stub SceCommonDialog_stub SceTouch_stub SceHid_stub SceMotion_stub
  ScePower_stub SceProcessmgr_stub SceCamera_stub SceKernelThreadMgr_stub SceNet_stub SceNetCtl_stub)
# vitaGL and the Cg (SHARK) runtime the shader generator uses at runtime.
set(TPV_VITA_GL_LIBS
  vitaGL mathneon vitashark SceShaccCgExt taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
  # libvitaGL.a references sceGxm*; static stub archives are order-sensitive, so SceGxm_stub must come
  # AFTER vitaGL even though it is already in TPV_VITA_SYS_LIBS (checked: removing this entry gives
  # undefined references from libvitaGL.a). SceGpuEs4_stub / libScePiglet_stub are not needed.
  SceGxm_stub)
target_link_libraries(tpv_vita PRIVATE
  tpv_game tpv_jsystem ${TPV_GAME_LIBS} tpv_borealis tpv_aurora
  SDL3 ${TPV_VITA_SYS_LIBS} ${TPV_VITA_GL_LIBS})

# Make libstdc++'s "is threading available" probe resolve, so std::thread and std::mutex are real.
#
# Why this is needed at all: libstdc++ decides at LINK time whether threading exists. Its prebuilt
# thread.o/guard.o contain a weak reference to pthread_cancel and __gthread_active_p() returns
# "&pthread_cancel != 0". A weak reference never pulls a member out of a static archive, so unless
# something else references pthread_cancel strongly, the address stays 0 and libstdc++ concludes the
# program is single-threaded. Then:
#   - std::thread's constructor throws std::system_error(EPERM), "Enable multithreading to use
#     std::thread: Not owner". JKRThread::resume() -> OSResumeThread() constructs a std::thread, so
#     every JKRThread creation aborted. That is what killed JKRCreateAramStreamManager inside
#     JFWSystem::init().
#   - __gthread_mutex_lock()/unlock() become no-ops, so std::mutex silently does nothing.
#
# `nm` on the pre-fix ELF showed exactly four weak-undefined pthread symbols, i.e. exactly the ones
# nothing referenced strongly: pthread_cancel, pthread_rwlock_rdlock, pthread_rwlock_unlock,
# pthread_rwlock_wrlock. Naming those four (and only those) turns them into real undefined
# references, so the linker extracts pthread_cancel.o and friends from libpthread.a. Anything more
# is counter-productive: --whole-archive -lpthread and -pthread both fail here with duplicate
# definitions, because the VitaSDK spec already expands -pthread into a whole-archive pair.
#
# This also makes the -1 sentinel legal. VitaSDK's PTHREAD_MUTEX_INITIALIZER is
# ((pthread_mutex_t) -1), and its pthread_mutex_lock() disassembles to: load the first word; if it
# is 0 return EINVAL(22); if it is -1 call pte_mutex_check_need_init() and lock lazily. So a
# statically initialised mutex works once real pthread is linked, whereas a zeroed one would return
# EINVAL and std::mutex::lock() would throw std::system_error(22).
# Crash symbol resolution. The map is produced by the same link that produces the ELF, so the table
# it yields describes the previous build on the first pass and the current one thereafter. That is
# deliberate: a stale table is only ever consulted when the process is already aborting, and the
# alternative (linking twice on every build) costs minutes. The table is regenerated below whenever
# the map changes, so a crash log names the symbols of the build that crashed.
set(TPV_CRASH_MAP "${CMAKE_CURRENT_BINARY_DIR}/tpv_vita.map")
set(TPV_CRASH_TABLE "${CMAKE_CURRENT_BINARY_DIR}/generated/crash_map.inc")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_options(tpv_vita PRIVATE
  -Wl,--undefined=pthread_once
  -Wl,--undefined=pthread_cancel
  -Wl,--undefined=pthread_rwlock_rdlock
  -Wl,--undefined=pthread_rwlock_unlock
  -Wl,--undefined=pthread_rwlock_wrlock
  # Catch every abort() in the process without editing dusklight: newlib defines abort as a strong
  # global, so --wrap redirects all call sites to __wrap_abort in vita/src/crash_record.cpp.
  # __assert_func is wrapped too, because a failed assert otherwise loses its expression and line
  # before the log can be flushed.
  -Wl,--wrap=abort
  -Wl,--wrap=__assert_func
  -Wl,-Map=${TPV_CRASH_MAP})

# Compile the (possibly empty) crash table in. crash_symbols.cpp #includes it as a string-literal body.
target_include_directories(tpv_vita PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# After each link: turn the map into the table for the NEXT build. Kept off the critical path.
add_custom_command(TARGET tpv_vita POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${TPV_DIR}/tools/map_symbols.py ${TPV_CRASH_MAP} ${TPV_CRASH_TABLE}.next
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TPV_CRASH_TABLE}.next ${TPV_CRASH_TABLE}
  COMMENT "Regenerating crash symbol table from the link map"
  VERBATIM)


# Static libs are order-sensitive: libstdc++ (std::condition_variable -> pthread_cond_*) and bzip2
# (used by nod_lite/zstd users) must come after everything that references them. The trailing
# -lpthread matters: libstdc++'s guard.o/tzdb.o emit pthread_cond_* references that the abseil-supplied
# -lpthread earlier on the line has already passed.
set(CMAKE_CXX_STANDARD_LIBRARIES "-lbz2 -lpng16 -lz -lstdc++ -lpthread -lm")

# vita-elf-create needs ~5 KiB behind the text segment, but the linker script puts the data segment
# at the next 64 KiB boundary, so whether it fits depends on the text size modulo 64 KiB (see
# vita/src/text_pad.cpp). The pad moves the text end away from a boundary; the check fails the link
# step early, with the remedy, when a later code change lands in the window again.
set(TPV_TEXT_PAD_KIB 32 CACHE STRING "KiB of .rodata filler that keeps vita-elf-create's SCE data from overlapping the data segment")
set_source_files_properties(${TPV_DIR}/vita/src/text_pad.cpp PROPERTIES COMPILE_DEFINITIONS TPV_TEXT_PAD_KIB=${TPV_TEXT_PAD_KIB})
add_custom_command(TARGET tpv_vita POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${TPV_DIR}/tools/check_text_gap.py $<TARGET_FILE:tpv_vita>
  COMMENT "Checking room for SCE data behind the text segment")

# --- Packaging ----------------------------------------------------------------
# The SELF/VPK targets are opt-in (-DTPV_PACKAGE=ON). The VPK builds and passes check_vpk.py, but
# nothing has been run on a Vita or in Vita3K yet.
option(TPV_PACKAGE "Create the SELF and VPK" OFF)
if(TPV_PACKAGE)
  vita_create_self(tpv_vita.self tpv_vita)
  vita_create_vpk(tpv_vita.vpk ${VITA_TITLEID} tpv_vita.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
endif()
