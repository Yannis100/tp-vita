# Aurora's GX frontend (GX API, FIFO command processor, BP/XF register decode, display-list parser)
# with the WebGPU backend swapped for a vitaGL one.
#
# tools/build_aurora_tree.py assembles build/aurora_lib = upstream lib/ + vita/aurora/overlay/*
# (whole-file replacements/additions) + vita/aurora/patches.py (string patches). Only files that are
# WebGPU-free after that are compiled.

set(TPV_ALIB ${CMAKE_CURRENT_BINARY_DIR}/aurora_lib)
file(GLOB_RECURSE _tpv_overlay_files CONFIGURE_DEPENDS ${TPV_DIR}/vita/aurora/overlay/*)
# Content changes of any overlay file must re-run the tree assembly below. CONFIGURE_DEPENDS on the
# glob only notices files being added or removed; without listing the files here an edited overlay
# source is compiled from the stale copy in build/aurora_lib (the VPK silently misses the change).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${TPV_DIR}/tools/build_aurora_tree.py ${TPV_DIR}/vita/aurora/patches.py ${_tpv_overlay_files})
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${TPV_DIR}/tools/build_aurora_tree.py
          ${AURORA_DIR}/lib ${TPV_DIR}/vita/aurora/overlay ${TPV_DIR}/vita/aurora/patches.py ${TPV_ALIB}
  RESULT_VARIABLE _tree_rc OUTPUT_VARIABLE _tree_out ERROR_VARIABLE _tree_err)
if(NOT _tree_rc EQUAL 0)
  message(FATAL_ERROR "build_aurora_tree.py failed:\n${_tree_out}${_tree_err}")
endif()
message(STATUS "${_tree_out}")

set(_G ${TPV_ALIB})
set(TPV_AURORA_GX_SOURCES
  # GX API (dolphin/gx/*)
  ${_G}/dolphin/gx/GXAurora.cpp ${_G}/dolphin/gx/GXBump.cpp ${_G}/dolphin/gx/GXCpu2Efb.cpp
  ${_G}/dolphin/gx/GXCull.cpp ${_G}/dolphin/gx/GXDispList.cpp ${_G}/dolphin/gx/GXDraw.cpp
  ${_G}/dolphin/gx/GXExtra.cpp ${_G}/dolphin/gx/GXFifo.cpp ${_G}/dolphin/gx/GXFrameBuffer.cpp
  ${_G}/dolphin/gx/GXGeometry.cpp ${_G}/dolphin/gx/GXGet.cpp ${_G}/dolphin/gx/GXLighting.cpp
  ${_G}/dolphin/gx/GXManage.cpp ${_G}/dolphin/gx/GXPerf.cpp ${_G}/dolphin/gx/GXPixel.cpp
  ${_G}/dolphin/gx/GXTev.cpp ${_G}/dolphin/gx/GXTexture.cpp ${_G}/dolphin/gx/GXTransform.cpp
  ${_G}/dolphin/gx/GXVert.cpp
  # FIFO, register decode, command processor, display lists
  ${_G}/gx/fifo.cpp ${_G}/gx/regs.cpp ${_G}/gx/command_processor.cpp ${_G}/gx/shader_info.cpp
  ${_G}/gx/attr_fmt.cpp ${_G}/gx/dl.cpp ${_G}/gx/gx.cpp ${_G}/gx/texture.cpp
  # Vita backend
  ${_G}/gx/vita_vertex.cpp ${_G}/gx/vita_shader.cpp
  ${_G}/gfx/vita_frame.cpp ${_G}/gfx/vita_gl_replay.cpp ${_G}/gfx/vita_texture.cpp
  ${_G}/gfx/vita_texture_convert.cpp
)

target_sources(tpv_aurora PRIVATE ${TPV_AURORA_GX_SOURCES})
target_include_directories(tpv_aurora PRIVATE ${TPV_ALIB})
