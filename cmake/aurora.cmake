# Aurora (GameCube/Wii SDK compatibility layer) for Vita.
# Platform-neutral parts compile straight from the Dusklight checkout. Files that are bound to
# WebGPU/Dawn, SDL windowing or nod are replaced by versions in vita/aurora/.

set(_A ${AURORA_DIR}/lib)
# EVERY Aurora source in this file is compiled from the merged tree, not from ${_A}.
# build_aurora_tree.py assembles build/aurora_lib = upstream lib/ + vita/aurora/overlay/* +
# vita/aurora/patches.py, and patches.py is how this port changes upstream Aurora at all. Compiling
# ${_A} directly meant a patch.py entry whose file appeared here was silently ignored: that is exactly
# what happened to dolphin/dvd/dvd.cpp (the whole DVD-worker patch set did nothing) and to
# dolphin/os/OSTime.cpp (the std::call_once fix could never have applied).
# TPV_ALIB is defined here rather than in aurora_gx.cmake because this file is included first.
if(NOT DEFINED TPV_ALIB)
  set(TPV_ALIB ${CMAKE_CURRENT_BINARY_DIR}/aurora_lib)
endif()
set(TPV_AURORA_UPSTREAM
  # core
  ${TPV_ALIB}/logging.cpp ${TPV_ALIB}/io.cpp ${TPV_ALIB}/thread.cpp ${TPV_ALIB}/time.cpp ${TPV_ALIB}/device.cpp
  # input: a single input.cpp at aurora 3227d76. (aurora 505b5eb splits this into input/ and deletes
  # most of the PAD mapping API, which Dusklight's UI still uses - see vita/aurora/patches.py.)
  ${TPV_ALIB}/input.cpp
  # os
  ${TPV_ALIB}/dolphin/os/OSInit.cpp ${TPV_ALIB}/dolphin/os/OSCache.cpp ${TPV_ALIB}/dolphin/os/OSMemory.cpp
  ${TPV_ALIB}/dolphin/os/OSBootInfo.cpp ${TPV_ALIB}/dolphin/os/OSTime.cpp ${TPV_ALIB}/dolphin/os/OSArena.cpp
  ${TPV_ALIB}/dolphin/os/OSAlloc.cpp ${TPV_ALIB}/dolphin/os/OSAddress.cpp ${TPV_ALIB}/dolphin/os/OSReport.cpp
  ${TPV_ALIB}/dolphin/AR.cpp
  # mtx
  ${TPV_ALIB}/dolphin/mtx/mtx.c ${TPV_ALIB}/dolphin/mtx/mtxstack.c ${TPV_ALIB}/dolphin/mtx/mtxvec.c
  ${TPV_ALIB}/dolphin/mtx/mtx44.c ${TPV_ALIB}/dolphin/mtx/vec.c ${TPV_ALIB}/dolphin/mtx/quat.c
  # gd
  ${TPV_ALIB}/dolphin/gd/GDBase.cpp ${TPV_ALIB}/dolphin/gd/GDGeometry.cpp ${TPV_ALIB}/dolphin/gd/GDIndirect.cpp
  ${TPV_ALIB}/dolphin/gd/GDLight.cpp ${TPV_ALIB}/dolphin/gd/GDPixel.cpp ${TPV_ALIB}/dolphin/gd/GDTev.cpp
  ${TPV_ALIB}/dolphin/gd/GDTexture.cpp ${TPV_ALIB}/dolphin/gd/GDTransform.cpp ${TPV_ALIB}/dolphin/gd/GDAurora.cpp
  # si / vi / thp
  ${TPV_ALIB}/dolphin/si/si.cpp ${TPV_ALIB}/dolphin/vi/vi.cpp ${TPV_ALIB}/dolphin/pad/pad.cpp
  ${TPV_ALIB}/dolphin/thp/THPAudio.cpp ${TPV_ALIB}/dolphin/thp/THPDec.cpp
  # card
  ${TPV_ALIB}/card/BlockAllocationTable.cpp ${TPV_ALIB}/card/CardRawFile.cpp ${TPV_ALIB}/card/CardGciFolder.cpp
  ${TPV_ALIB}/card/Directory.cpp ${TPV_ALIB}/card/DolphinCardPath.cpp ${TPV_ALIB}/card/File.cpp ${TPV_ALIB}/card/FileIO.cpp
  ${TPV_ALIB}/card/SRAM.cpp ${TPV_ALIB}/card/Util.cpp ${TPV_ALIB}/dolphin/card.cpp
  # dvd (disc access goes through nod_lite instead of the Rust nod library)
  ${TPV_ALIB}/dolphin/dvd/dvd.cpp ${TPV_ALIB}/dolphin/dvd/fst.cpp
)

add_library(nod_lite STATIC ${TPV_DIR}/vita/nod_lite/nod_lite.c)
target_include_directories(nod_lite PUBLIC ${TPV_DIR}/vita/nod_lite)

add_library(tpv_aurora STATIC ${TPV_AURORA_UPSTREAM})
target_compile_definitions(tpv_aurora PUBLIC AURORA TARGET_PC AURORA_ENABLE_GX AURORA_ENABLE_RMLUI TARGET_VITA=1)
target_include_directories(tpv_aurora PUBLIC ${TPV_DIR}/vita/include ${AURORA_DIR}/include)
target_link_libraries(tpv_aurora PUBLIC fmt::fmt xxHash::xxhash SDL3 Tracy::TracyClient
  absl::btree absl::flat_hash_map absl::flat_hash_set zstd::libzstd nod_lite)
