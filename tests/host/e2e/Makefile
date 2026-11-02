# End-to-end host render harness: the real GX API -> FIFO -> command processor -> recorded frame -> GL replay,
# compiled for x86 against Mesa llvmpipe (EGL surfaceless + GLES2). Drive it with run.sh.
#
#   AURORA_DIR   upstream Aurora checkout (default ../dusklight/extern/aurora next to this repo)
#   TPV_E2E_OBJ  object directory (default /tmp/tpv/e2e/obj)
# build/aurora_lib must be current (run.sh assembles it).
ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
HERE    := $(ROOT)/tests/host/e2e
G       := $(ROOT)/build/aurora_lib
AURORA_DIR ?= $(ROOT)/../dusklight/extern/aurora
OBJ     ?= /tmp/tpv/e2e/obj
GLINC   ?= /tmp/tpv/hostgl/inc
OUT     ?= /tmp/tpv/e2e/e2e
ABSL    := $(ROOT)/deps/abseil
# Game code the scenes call directly (display-list fog writer): the real file from the Dusklight checkout.
DUSK    ?= $(abspath $(AURORA_DIR)/../..)
GFPIXEL_SRC ?= $(DUSK)/libs/dolphin/src/gf/GFPixel.cpp

CXXFLAGS ?= -O1 -g
LDFLAGS  ?=
INCS := -I$(GLINC) -I$(ROOT)/tests/host -I$(AURORA_DIR)/include -I$(G) -I$(ROOT)/deps/xxhash \
        -I$(ROOT)/deps/tracy/public -I$(ABSL)
# EGL_NO_X11: the harness needs EGL and GLES2 only; without it EGL/eglplatform.h includes <X11/Xlib.h> and the build needs libx11-dev.
DEFS := -DFMT_HEADER_ONLY=1 -DAURORA -DAURORA_ENABLE_GX -DTARGET_PC -DTARGET_VITA=1 -DEGL_NO_X11
# -MMD/-MP: header edits (the overlay headers are the renderer contract) must rebuild the objects that include them.
ALLFLAGS := -std=gnu++20 -w -MMD -MP $(DEFS) $(INCS) $(CXXFLAGS)

PIPE := dolphin/gx/GXAurora dolphin/gx/GXBump dolphin/gx/GXCpu2Efb dolphin/gx/GXCull dolphin/gx/GXDispList \
  dolphin/gx/GXDraw dolphin/gx/GXExtra dolphin/gx/GXFifo dolphin/gx/GXFrameBuffer dolphin/gx/GXGeometry \
  dolphin/gx/GXGet dolphin/gx/GXLighting dolphin/gx/GXManage dolphin/gx/GXPerf dolphin/gx/GXPixel \
  dolphin/gx/GXTev dolphin/gx/GXTexture dolphin/gx/GXTransform dolphin/gx/GXVert \
  gx/fifo gx/regs gx/command_processor gx/shader_info gx/attr_fmt gx/dl gx/gx gx/texture \
  gx/vita_vertex gx/vita_shader gfx/vita_frame gfx/vita_gl_replay gfx/vita_texture gfx/vita_texture_convert
# The same list the Vita build compiles: cmake/aurora_gx.cmake (TPV_AURORA_GX_SOURCES).
ABSLSRC := base/internal/raw_logging base/log_severity hash/internal/hash hash/internal/city \
  hash/internal/low_level_hash container/internal/raw_hash_set

OBJS := $(addprefix $(OBJ)/pipe/,$(addsuffix .o,$(subst /,_,$(PIPE)))) \
        $(addprefix $(OBJ)/absl/,$(addsuffix .o,$(subst /,_,$(ABSLSRC)))) \
        $(OBJ)/xxhash.o $(OBJ)/glctx.o $(OBJ)/e2e_stubs.o $(OBJ)/game_GFPixel.o $(OBJ)/e2e_main.o

$(OUT): $(OBJS)
	@mkdir -p $(dir $@)
	g++ $(LDFLAGS) $(OBJS) -o $@ /usr/lib/x86_64-linux-gnu/libEGL.so.1 /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 -lz -lpthread

define PIPE_RULE
$(OBJ)/pipe/$(subst /,_,$(1)).o: $(G)/$(1).cpp
	@mkdir -p $$(dir $$@)
	g++ $$(ALLFLAGS) -c $$< -o $$@
endef
$(foreach s,$(PIPE),$(eval $(call PIPE_RULE,$(s))))

define ABSL_RULE
$(OBJ)/absl/$(subst /,_,$(1)).o: $(ABSL)/absl/$(1).cc
	@mkdir -p $$(dir $$@)
	g++ $$(ALLFLAGS) -c $$< -o $$@
endef
$(foreach s,$(ABSLSRC),$(eval $(call ABSL_RULE,$(s))))

$(OBJ)/xxhash.o: $(ROOT)/deps/xxhash/xxhash.c
	@mkdir -p $(dir $@)
	gcc -O2 -w -c $< -o $@
$(OBJ)/glctx.o: $(ROOT)/tests/host/glctx.cpp
	g++ $(ALLFLAGS) -c $< -o $@
$(OBJ)/e2e_stubs.o: $(HERE)/e2e_stubs.cpp
	g++ $(ALLFLAGS) -c $< -o $@
# The game's headers come after Aurora's, so only this file sees them. Rebuilt whenever GFPIXEL_SRC (or a header) is newer.
$(OBJ)/game_GFPixel.o: $(GFPIXEL_SRC)
	g++ $(ALLFLAGS) -I$(DUSK)/libs/dolphin/include -c $< -o $@
$(OBJ)/e2e_main.o: $(HERE)/e2e_main.cpp $(wildcard $(HERE)/*.hpp)
	g++ $(ALLFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)
