# libgl in an app's Makefile: include it before mk/maxsdk.mk (it adds
# its HLSL program to HLSL_DIRS), with OUT set:
#
#   MXGL ?= $(PS2MAX)/libs/libgl
#   include $(MXGL)/mk/mxgl.mk
#   include $(MAXSDK)/mk/maxsdk.mk
#   ... -I$(MXGL_INC) in the app's CFLAGS; $(MXGL_OBJS) in the link
#
# MXGL_OBJS is $(OUT)/mxgl/mxgl.o; it needs the HLSL objects
# ($(HLSL_OBJS)) in the link too.

MXGL_DIR  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
MXGL_INC  := $(MXGL_DIR)/include
HLSL_DIRS += $(MXGL_DIR)/src
# MXGL_PROGS: the app's vertex programs (NAME.hlsl in HLSL_DIRS) for
# glShaderBinary: NAME_vu1 in $(OUT)/mxgl/NAME_vu1.o (<mxgl.h>)
# MXGL_VU1CLIP=1: clipping and culling on VU1 (src/mxglc.vsm)
ifeq ($(MXGL_VU1CLIP),1)
VSM_DIRS  += $(MXGL_DIR)/src
MXGL_VU1  := $(OUT)/vu/mxglc.o $(OUT)/vu/mxglcf.o $(OUT)/vu/mxglce.o
endif
MXGL_OBJS  = $(OUT)/mxgl/mxgl.o $(MXGL_VU1) $(patsubst %,$(OUT)/mxgl/%_vu1.o,$(MXGL_PROGS) $(MXGL_VSM_PROGS)) \
             $(patsubst %,$(OUT)/vu/%.o,$(MXGL_VSM_PROGS))
# MXGL_VSM_PROGS: hand-written vertex programs for glShaderBinary, the same way:
# NAME.vsm and NAME.h, its layout (the macros of a compiled program's header,
# VUEntryABI.md), in MXGL_VSM_DIR; src/mxglc.i, with clipping on VU1, is included as
# "mxglc.i"
VSM_FLAGS += -I$(MXGL_DIR)/src
VSM_DIRS  += $(MXGL_VSM_DIR)
# MXGL_MAX_TEXTURES: the number of texture names (2048 by default: about 500 KB
# of static memory; a program with a few textures can do with less)
# MXGL_ARENA_BYTES: the size of each frame arena (512 KB by default; two,
# four with beam racing); MXGL_REC_BYTES: beam racing's record of a frame's
# stream (256 KB by default, four of them; a quarter of each is the
# uploads'); the app's sources may add MXGL_DEFS too
MXGL_DEFS  := $(if $(MXGL_ARENA_BYTES),-DMXGL_ARENA_BYTES=$(MXGL_ARENA_BYTES)) \
              $(if $(MXGL_REC_BYTES),-DMXGL_REC_BYTES=$(MXGL_REC_BYTES)) \
              $(if $(filter 1,$(MXGL_VU1CLIP)),-DMXGL_VU1CLIP=1) \
              $(if $(MXGL_MAX_TEXTURE),-DMXGL_MAX_TEXTURE=$(MXGL_MAX_TEXTURE)) \
              $(if $(MXGL_MAX_TEXTURES),-DMXGL_MAX_TEXTURES=$(MXGL_MAX_TEXTURES)) \
              $(if $(MXGL_FB_W),-DMXGL_FB_W=$(MXGL_FB_W)) \
              $(if $(filter 1,$(MXGL_FB_Z16)),-DMXGL_FB_Z=MX_GS_PSM_Z16S) \
              $(if $(MXGL_STATS_WINDOW),-DMXGL_STATS_WINDOW=$(MXGL_STATS_WINDOW))
# VU0 macro mode for the vector code (v4f), fast float math, float
# literals (the FPU has no doubles), builtins (not -ffreestanding: sqrtf is
# sqrt.s, small memcpy inline); picolibc's headers (malloc.h, math.h)
MXGL_CFLAGS = -I$(MXGL_DIR)/src $(MXGL_DEFS) -mvu0 -ffast-math -fapprox-func -cl-single-precision-constant -MMD -I$(MXGL_INC) \
              -nostdlibinc -isystem $(or $(PICOLIBC),$(PS2MAX)/build/libs/picolibc/install)/include

# mxgl.c includes its parts (src/gl/*.c): one translation unit
$(OUT)/mxgl/mxgl.o: $(MXGL_DIR)/src/mxgl.c $(wildcard $(MXGL_DIR)/src/gl/*.c) $(MXGL_INC)/mxgl.h $(OUT)/hlsl/mxglv.h \
                    $(OUT)/hlsl/mxglvf.h $(OUT)/hlsl/mxglve.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -ffreestanding,$(CFLAGS)) $(MXGL_CFLAGS) -c $< -o $@

$(OUT)/mxgl/%_vu1.c: $(OUT)/hlsl/%.h $(MXGL_DIR)/mk/vu1bin.awk
	@mkdir -p $(dir $@)
	awk -v name=$* -f $(MXGL_DIR)/mk/vu1bin.awk $< > $@

$(patsubst %,$(OUT)/mxgl/%_vu1.c,$(MXGL_VSM_PROGS)): $(OUT)/mxgl/%_vu1.c: $(MXGL_VSM_DIR)/%.h $(MXGL_DIR)/mk/vu1bin.awk
	@mkdir -p $(dir $@)
	awk -v name=$* -f $(MXGL_DIR)/mk/vu1bin.awk $< > $@

$(OUT)/vu/mxglc.vu.elf $(OUT)/vu/mxglcf.vu.elf $(OUT)/vu/mxglce.vu.elf $(patsubst %,$(OUT)/vu/%.vu.elf,$(MXGL_VSM_PROGS)): $(MXGL_DIR)/src/mxglc.i

$(OUT)/mxgl/%_vu1.o: $(OUT)/mxgl/%_vu1.c
	$(CC) $(filter-out -ffreestanding,$(CFLAGS)) $(MXGL_CFLAGS) -I$(OUT)/hlsl \
	  $(addprefix -I,$(MXGL_VSM_DIR)) -c $< -o $@
