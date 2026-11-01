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
MXGL_OBJS  = $(OUT)/mxgl/mxgl.o
# MXGL_ARENA_BYTES: the size of each frame arena (512 KB by default; two,
# four in 1080i); the app's sources may add MXGL_DEFS too
MXGL_DEFS  := $(if $(MXGL_ARENA_BYTES),-DMXGL_ARENA_BYTES=$(MXGL_ARENA_BYTES))
# VU0 macro mode for the vector code (v4f), fast float math, float
# literals (the FPU has no doubles), builtins (not -ffreestanding: sqrtf is
# sqrt.s, small memcpy inline); picolibc's headers (malloc.h, math.h)
MXGL_CFLAGS = $(MXGL_DEFS) -mvu0 -ffast-math -fapprox-func -cl-single-precision-constant -MMD -I$(MXGL_INC) \
              -nostdlibinc -isystem $(or $(PICOLIBC),$(PS2MAX)/build/libs/picolibc/install)/include

$(OUT)/mxgl/mxgl.o: $(MXGL_DIR)/src/mxgl.c $(MXGL_INC)/mxgl.h $(OUT)/hlsl/mxglv.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -ffreestanding,$(CFLAGS)) $(MXGL_CFLAGS) -c $< -o $@
