# Tier-1 Vita skeleton for The Legend of Zelda: Twilight Princess (zeldaret/tp via TwilitRealm/dusklight)
#
# Compiles the portable game-logic core of the TP decomp natively for arm-vita-eabi
# (JMath, JSupport streams/lists, Dolphin mtx) with a Vita-side placeholder GX layer.
# NOT a playable port -- proves the game logic builds for ARM and is the seed for the
# future GX->GXM renderer.
#
# The game sources come from the upstream checkout. Set TP_DEPS to the dusklight repo
# root (containing include/, libs/JSystem, libs/revolution). If unset, it is discovered
# automatically as ../dusklight relative to this file.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(tp_vita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "TP Tier1 Skel")
set(VITA_TITLEID  "TPLG00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -std=gnu11 -g -Wall -fsigned-char")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17 -g -Wall -fsigned-char")
# C++20 features (std::is_constant_evaluated etc.) are used by the game's endian helpers:
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Upstream relies on a precompiled header to define global.h macros (DUSK_GAME_EXTERN,
# ATTRIBUTE_ALIGN) before any game header is parsed. Replicate with a forced include.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include ${CMAKE_CURRENT_SOURCE_DIR}/vita/include/global.h")

# Larger main memory budget (365 MiB instead of 256). The skeleton is small; kept on
# so a future full game-logic build has headroom.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

# --- Upstream TP/dusklight source tree -------------------------------------
if(NOT DEFINED TP_DEPS)
  get_filename_component(_here "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
  if(EXISTS "${_here}/../../../dusklight")
    set(TP_DEPS "${_here}/../../../dusklight")
  elseif(EXISTS "/home/yanserm1/psvita/dusklight")
    set(TP_DEPS "/home/yanserm1/psvita/dusklight")
  else()
    message(FATAL_ERROR "Set TP_DEPS to the dusklight repo root (contains include/, libs/JSystem)")
  endif()
endif()
message(STATUS "TP_DEPS=${TP_DEPS}")

# --- Portable game-logic sources from the TP decomp ------------------------
# JMath: pure math, JSystem.pch-free (upstream units are guarded for non-MWERKS).
set(TP_JMATH
  ${TP_DEPS}/libs/JSystem/src/JMath/JMath.cpp
  ${TP_DEPS}/libs/JSystem/src/JMath/random.cpp
  ${TP_DEPS}/libs/JSystem/src/JMath/JMATrigonometric.cpp
)
# JSupport: streams + lists that do NOT need JKernel (JSUFileStream is excluded).
set(TP_JSUPPORT
  ${TP_DEPS}/libs/JSystem/src/JSupport/JSUList.cpp
  ${TP_DEPS}/libs/JSystem/src/JSupport/JSUInputStream.cpp
  ${TP_DEPS}/libs/JSystem/src/JSupport/JSUOutputStream.cpp
  ${TP_DEPS}/libs/JSystem/src/JSupport/JSUMemoryStream.cpp
)

# --- Vita-side layer --------------------------------------------------------
# 1. 'vita/include' shadows the Aurora/Dolphin headers with declarations only.
# 2. 'vita/stubs' provides portable C implementations (mtx/vec/quat are pure PPC asm
#    upstream, so this is the required port).
# 3. 'vita/vita_runtime.c' sets heap/stack/clocks.
set(VITA_LAYER
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/stubs/mtx.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/stubs/ppc_math.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/stubs/os.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/stubs/jsupport_fix.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/vita_runtime.c
)

add_executable(${PROJECT_NAME}
  src/main.cpp
  ${TP_JMATH}
  ${TP_JSUPPORT}
  ${VITA_LAYER}
)

# Include order matters: our Vita headers FIRST (so <dolphin/os.h>, <global.h>,
# JSystem/JSystem.h resolve to the shadows), then the upstream game headers.
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/vita/include   # shadows: dolphin, global.h, JSystem/JSystem.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${TP_DEPS}/include                          # game logic headers (d, f_op, helpers/endian.h)
  ${TP_DEPS}/libs/JSystem/include             # JSystem headers (JMath, JSupport, JSystem.JSystem.h shadowed above)
  ${TP_DEPS}/libs/revolution/include          # revolution umbrella (shadowed by vita/include)
  ${TP_DEPS}/extern/aurora/include            # dolphin decls not shadowed (mtx/GeoTypes etc.)
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  __VITA_PORT__=1
  TARGET_PC=1        # 32-bit ARM: u32 = unsigned int; endian helpers use BSWAP path
  NDEBUG=1
  NDEBUG_DEFINED=1
  DEBUG_DEFINED=0
)

target_link_libraries(${PROJECT_NAME}
  SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
  SceAudio_stub SceGxm_stub ScePower_stub SceAppUtil_stub SceCommonDialog_stub
  -pthread
  m
)

# Safe eboot. libshacccg.suprx is NOT needed (no ShaccCg) and NOT packaged.
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

# Build:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Check:  python3 ~/.roo/skills/vita-port/scripts/check_vpk.py build/tp_vita.vpk
