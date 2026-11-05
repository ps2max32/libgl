# libgl's tests on the PS2 (PCSX2 or the real one): each feature drawn and
# read back with glReadPixels.
#
#   make [OUT=<dir>]     $(OUT)/glconf.elf
#   make run             glconf.elf in PCSX2 (software renderer: the GS's own results)

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
MXGL     ?= $(PS2MAX)/libs/libgl
OUT      ?= $(PS2MAX)/build/libs/libgl/tests
all: $(OUT)/glconf.elf $(OUT)/glvsm.elf $(OUT)/glstencil.elf $(patsubst %,$(OUT)/glscene%.elf,0 1 2 3)
# glprog.hlsl: a vertex program for glShaderBinary (MXGL_PROGS)
HLSL_DIRS   := $(HERE)
MXGL_PROGS  := glprog
# glvsm.vsm: a hand-written one (MXGL_VSM_PROGS)
MXGL_VSM_DIR   := $(HERE)
MXGL_VSM_PROGS := glvsm
include $(MXGL)/mk/mxgl.mk
include $(MAXSDK)/mk/maxsdk.mk

PICOLIBC ?= $(PS2MAX)/build/libs/picolibc/install
LIBC_LIBS = $(MAXSDK_OUT)/libc/glue.c.o $(MAXSDK_OUT)/libmxc.a $(PICOLIBC)/lib/libc.a \
            $(MX_LIB) $(MAXSDK_OUT)/librt.a
TCFLAGS   = $(filter-out -ffreestanding,$(CFLAGS)) -MMD -nostdlibinc -isystem $(PICOLIBC)/include \
            -I$(MXGL_INC) -I$(MAXSDK)/tests/ps2

$(OUT)/glconf.o: $(HERE)/glconf.c $(OUT)/hlsl/glprog.h
	@mkdir -p $(dir $@)
	$(CC) $(TCFLAGS) -c $< -o $@

$(OUT)/glconf.elf: $(OUT)/glconf.o $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OUT)/glconf.o $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

$(OUT)/glvsm.o: $(HERE)/glvsm.c
	@mkdir -p $(dir $@)
	$(CC) $(TCFLAGS) -c $< -o $@

$(OUT)/glvsm.elf: $(OUT)/glvsm.o $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $(OUT)/glvsm.o $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

$(OUT)/glstencil.o: $(HERE)/glstencil.c
	@mkdir -p $(dir $@)
	$(CC) $(TCFLAGS) -c $< -o $@

$(OUT)/glstencil.elf: $(OUT)/glstencil.o $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $< $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

# glstencilp<n>: the stencil and the colour mask in a picture, to compare between modes
# (glReadPixels has no beam racing): n is mxgl_set_mode's mode
$(OUT)/glstencilp%.o: $(HERE)/glstencilp.c
	@mkdir -p $(dir $@)
	$(CC) $(TCFLAGS) -DMODE=$* -c $< -o $@

$(OUT)/glstencilp%.elf: $(OUT)/glstencilp%.o $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $< $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

# glscene<n>: pictures to compare between builds (MXGL_VU1CLIP=1 or not)
$(OUT)/glscene%.o: $(HERE)/glscene.c
	@mkdir -p $(dir $@)
	$(CC) $(TCFLAGS) -DMODE=$* -c $< -o $@

$(OUT)/glscene%.elf: $(OUT)/glscene%.o $(MXGL_OBJS) $(HLSL_OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,$(MX_CRT0) $< $(MXGL_OBJS) $(HLSL_OBJS) $(LIBC_LIBS))

run: $(OUT)/glconf.elf
	PCSX2_RENDERER=sw $(PCSX2_RUN) $< $(or $(TIMEOUT),120)

clean:
	rm -rf $(OUT)

.PHONY: all run clean

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
