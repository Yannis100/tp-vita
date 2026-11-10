# Twilight Princess for PS Vita — Dusklight (TwilitRealm, CC0) game code on a Vita platform layer.
#
# Layout:
#   TP_DEPS          Dusklight checkout (branch `vita`): game code, JSystem, Aurora, Borealis.
#   deps/            third-party sources fetched by tools/fetch_deps.sh
#   vita/aurora/     Vita replacements for Aurora's WebGPU/SDL/nod backends (overlaid on Aurora's lib/)
#   vita/src/        Vita entry point, platform glue, stubs for desktop-only features
#
# Build:  tools/fetch_deps.sh && tools/build.sh release
#         tools/build.sh dev     (asserts on, -O1; for reproducing a failure in Vita3K)
#         tools/build.sh debug   (-O0, asserts on)

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

set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")   # 365 MiB main budget

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Build variants -----------------------------------------------------------
# Two configurations, and the difference between them is chosen, not inherited.
#
#   Release  -O3, NDEBUG, PARTIAL_DEBUG. Ships. Asserts compile out, so it is fast and it is the
#            build whose behaviour users see.
#   Dev      -O1, frame pointers, PARTIAL_DEBUG, and asserts ON. For reproducing a failure in
#            Vita3K and reading a crash log. NOT -O0: the emulator is already slow enough that an
#            unoptimised build may never reach the failure being chased, which defeats the point.
#   Debug    Same as Dev but -O0. Only when Dev is too slow or a specific frame needs to be exact.
#
# THE ONE THING THAT MUST NOT DIFFER IS STRUCT LAYOUT. CMake's stock Debug flags inject -DDEBUG,
# and dusklight adds members under `#if DEBUG` (d_meter_HIO.h, f_ap_game.h, d_map.h, ...). That
# would change sizeof() of game objects, so the dev build would be a *different program* from the
# one that ships and a reproduced bug might not be the shipped bug at all. -DDEBUG and -D_DEBUG are
# therefore stripped, and layout is held identical across all three variants. PARTIAL_DEBUG=1 is
# already in TPV_GAME_DEFS and is the safe half of the debug instrumentation.
#
# -g is unconditional (see add_compile_options). It costs nothing in the shipped VPK because
# vita_create_self strips it, and it is what makes addr2line and nm work on a crash.
string(TOUPPER "${CMAKE_BUILD_TYPE}" TPV_VARIANT_KEY)
if(TPV_VARIANT_KEY STREQUAL "DEBUG")
  set(TPV_VARIANT debug)
elseif(TPV_VARIANT_KEY STREQUAL "DEV")
  set(TPV_VARIANT dev)
else()
  set(TPV_VARIANT release)
endif()
string(TOUPPER "${TPV_VARIANT}" TPV_VARIANT_UPPER)
message(STATUS "TPV_VARIANT=${TPV_VARIANT}")

# Asserts: on in the dev variants, off when shipping. Overridable either way for one-off builds.
# Enabling them in Release is supported on purpose: `assert()` failing is a real invariant violation
# and __wrap___assert_func names it, but expect new deaths, so it is not the default.
if(TPV_VARIANT STREQUAL "release")
  set(_tpv_asserts_default OFF)
else()
  set(_tpv_asserts_default ON)
endif()
option(TPV_ENABLE_ASSERTS "Keep assert() live (NDEBUG undefined)" ${_tpv_asserts_default})

# Dev/Debug: no -DDEBUG, no -D_DEBUG, and no -DNDEBUG (which is what keeps asserts alive).
# Note `dev` maps onto CMAKE's Debug flags; the variant name is what users pick with a custom
# build type, so map it back to flags here rather than relying on the stock sets.
# How optimisation is controlled, and two traps that made earlier attempts silently wrong.
#
# Trap 1: the VitaSDK toolchain sets CMAKE_CXX_FLAGS_DEBUG to "-O0 -g -DDEBUG -D_DEBUG" and
# CMAKE_CXX_FLAGS_RELEASE to "-O3 -DNDEBUG" in its own cache entries. Those are appended AFTER
# add_compile_options, so a variant flag added as a compile option loses to them. It is also where
# -DDEBUG comes from, which is exactly the struct-layout change this project must avoid.
#
# Trap 2: CMake reads CMAKE_CXX_FLAGS_<UPPERCASE BUILD TYPE>. A custom build type such as "Dev" reads
# an all-empty CMAKE_CXX_FLAGS_DEV, so EVERY build-type flag is silently dropped -- the build is then
# neither optimised nor unoptimised by intent. This was measured: CMAKE_CXX_FLAGS_DEBUG held the
# right value while the generated compile line had no -O flag at all.
#
# So the build-type flags are cleared to empty, which removes the toolchain's defaults and -DDEBUG,
# and a single add_compile_options list becomes the only authority. One place to read, one place to
# change, and no ordering question.
set(TPV_OPT_FLAGS_RELEASE "-O3 -DNDEBUG")
set(TPV_OPT_FLAGS_DEV     "-O1 -fno-omit-frame-pointer")
set(TPV_OPT_FLAGS_DEBUG   "-O0")
foreach(_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_C_FLAGS_${_cfg}   "" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_${_cfg} "" CACHE STRING "" FORCE)
endforeach()
# -g is added once, for every variant: it costs nothing in the shipped VPK (vita_create_self strips
# it) and it is what makes addr2line and the crash table work. No -fno-exceptions/-fno-rtti:
# dusklight throws and catches in achievements, archive, config and save_manager.
#
# separate_arguments is essential. add_compile_options keeps a string with spaces as ONE argument,
# so the compiler received a single "-O1 -fno-omit-frame-pointer" token and rejected it with
# "argument to '-O' should be a non-negative integer". Splitting first yields separate flags.
separate_arguments(_tpv_opt_list NATIVE_COMMAND "${TPV_OPT_FLAGS_${TPV_VARIANT_UPPER}}")
add_compile_options(${_tpv_opt_list} -g)
add_link_options(-g)
message(STATUS "TPV flags: ${_tpv_opt_list} -g")

# Distinct identity per variant, so a dev VPK can sit beside a release one on the same console and
# a screenshot or log can never be attributed to the wrong build. Title IDs must be exactly 9
# characters (check_vpk.py enforces it).
# Title IDs must be 4 uppercase letters followed by 5 digits (check_vpk.py enforces it), so the
# variant goes in the digit field: 00001 release, 10001 dev, 20001 debug. Readable, distinct, and
# accepted by the packager.
if(TPV_VARIANT STREQUAL "release")
  set(VITA_APP_NAME "Twilight Princess")
  set(VITA_TITLEID  "TPLG00001")   # what ships
elseif(TPV_VARIANT STREQUAL "dev")
  set(VITA_APP_NAME "Twilight Princess (dev)")
  set(VITA_TITLEID  "TPLG10001")
else()
  set(VITA_APP_NAME "Twilight Princess (debug)")
  set(VITA_TITLEID  "TPLG20001")
endif()
message(STATUS "TPV title id=${VITA_TITLEID} name='${VITA_APP_NAME}' asserts=${TPV_ENABLE_ASSERTS}")

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
  -fsigned-char -fno-short-enums
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
target_compile_definitions(tpv_jsystem PRIVATE ${TPV_GAME_DEFS} ${TPV_EXTRA_DEFS})
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
target_compile_definitions(tpv_game PRIVATE ${TPV_GAME_DEFS} ${TPV_EXTRA_DEFS})
target_include_directories(tpv_game PRIVATE ${TPV_GAME_INCLUDES})
target_link_libraries(tpv_game PRIVATE ${TPV_GAME_LIBS})
target_precompile_headers(tpv_game PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${TP_DEPS}/include/dusk_pch.hpp>")
# NDEBUG previously lived here as an unconditional "NDEBUG=1", which is why assert() could never
# fire whatever the build type said. It now follows TPV_ENABLE_ASSERTS. DEBUG is pinned to 0 in
# every variant on purpose: turning it on adds members under `#if DEBUG` and would change struct
# layout, which must not differ between the build that reproduces a bug and the build that ships.
if(TPV_ENABLE_ASSERTS)
  set(TPV_NDEBUG_DEFS "NDEBUG_DEFINED=0")
else()
  set(TPV_NDEBUG_DEFS "NDEBUG=1;NDEBUG_DEFINED=1")
endif()

# Dev-only instrumentation. Each is a separate switch rather than "dev implies everything", because
# on an emulator that is already slow these can push a reproduction run below the frame rate where
# the original bug ever appeared. The vertex cache in particular is measured as a wash here
# (HANDOVER 7.9), so enabling it by default in dev would add noise to a performance investigation
# rather than help it.
option(TPV_DEV_VERTEX_CACHE "Force the vertex decode cache on in dev builds (TPV_VERTEX_CACHE=1)" OFF)
option(TPV_DEV_BOOT_TRACE   "Verbose boot/DVD tracing in dev builds (TPV_BOOT_TRACE=1)" OFF)
set(TPV_EXTRA_DEFS "")
if(TPV_ENABLE_ASSERTS)
  # A dev build says so at the top of its own log, so a crash log and a screenshot can be attributed
  # to the right variant without guessing from the title id.
  list(APPEND TPV_EXTRA_DEFS "TPV_DEV_BUILD=1")
endif()
if(TPV_DEV_VERTEX_CACHE)
  list(APPEND TPV_EXTRA_DEFS "TPV_VERTEX_CACHE=1")
endif()
if(TPV_DEV_BOOT_TRACE)
  list(APPEND TPV_EXTRA_DEFS "TPV_BOOT_TRACE=1")
endif()

set_source_files_properties(${DOLZEL_FILES} ${Z2AUDIOLIB_FILES} ${REL_FILES} ${DOLPHIN_FILES} ${_dusk_files}
  TARGET_DIRECTORY tpv_game PROPERTIES COMPILE_DEFINITIONS "${TPV_NDEBUG_DEFS};DEBUG_DEFINED=0;DEBUG=0")

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
  # Frame-counted input injection for automated runs (inert without ux0:data/tpvita/input_script.txt).
  ${TPV_DIR}/vita/src/input_script.cpp
  ${TPV_DIR}/vita/aurora/backend.cpp
  ${TPV_DIR}/vita/aurora/aurora_stubs.cpp
  ${TPV_DIR}/vita/aurora/imgui_vita.cpp
)

# The game provides aurora_main (from dusk/main.cpp via the main.h rename).
# We link its own `main` entry point that forwards to aurora_main.
add_executable(tpv_vita ${TPV_VITA_SOURCES})
target_compile_definitions(tpv_vita PRIVATE ${TPV_GAME_DEFS} ${TPV_EXTRA_DEFS})
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
# Crash symbol resolution. The table is a FILE shipped in the VPK, not a blob compiled in, for two
# measured reasons: a complete text-symbol table is ~4.2 MiB and .text+.rodata has ~118 KiB of slack
# before the data segment, and a compiled-in table is always one link stale because it can only be
# built from the ELF that just finished linking. See vita/src/crash_symbols.cpp.
#
# It is generated from the ELF, so it describes exactly the image in the VPK. Everything that names
# an address checks the table's anchor address against the live one first, so a table left over from
# another build is refused rather than trusted.
set(TPV_CRASH_TABLE "${CMAKE_CURRENT_BINARY_DIR}/crash_symbols.txt")
set(TPV_CRASH_ELF "${CMAKE_CURRENT_BINARY_DIR}/tpv_vita")

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
  -Wl,--wrap=__assert_func)

# After each link: turn the ELF's symbol table into the table that ships beside it. nm is used rather
# than the linker map because the map records neither symbol sizes nor the demangled names with
# spaces, and both gaps produced a wrong answer on a real crash.
add_custom_command(TARGET tpv_vita POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${TPV_DIR}/tools/map_symbols.py ${TPV_CRASH_ELF} ${TPV_CRASH_TABLE} ${CMAKE_NM}
  COMMENT "Generating crash symbol table from the linked ELF"
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
# The VPK name carries the variant. A release VPK and a dev VPK are different programs with
# different title ids, and naming them both tpv_vita.vpk guarantees that eventually a screenshot,
# a frame count or a crash log gets attributed to the wrong one. Build output is therefore
# tpv_vita-release.vpk / tpv_vita-dev.vpk / tpv_vita-debug.vpk.
if(TPV_PACKAGE)
  vita_create_self(tpv_vita.self tpv_vita)
  # The crash symbol table rides in the VPK at app0:data/tpvita/. It is ~4 MiB of text that is only
  # opened if the process dies, and it is why a crash log can name the function that aborted.
  vita_create_vpk(tpv_vita-${TPV_VARIANT}.vpk ${VITA_TITLEID} tpv_vita.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE ${TPV_CRASH_TABLE} data/tpvita/crash_symbols.txt)
endif()
