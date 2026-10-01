#
# iortcw - single top-level Makefile (Linux, Dreamcast)
#
#   make sp            build single player  -> build/sp-x86/iowolfsp.x86
#   make               the same
#   make clean         remove build/
#   make pvrtest       build the tools/gpu_pvr smoke test
#   make assets        Dreamcast versions of the game data (tools/rtcwconv):
#                      sp_dc.pk3, written to ASSETS_DIR
#                      (default assets/main, next to the original pk3s);
#                      images need PVRTEX (KOS utils/pvrtex)
#
#   make sp PLATFORM=dc   Dreamcast build with KallistiOS -> build/sp-sh4-.../iowolfsp.elf
#                         (source /opt/toolchains/dc/kos/environ.sh first)
#   make disc          Dreamcast build plus the sp pk3s (with sp_dc.pk3 if
#                      made) in a selfboot disc image -> build/iowolfsp.cdi
#                      (needs mkdcdisc; sources KOS's environ.sh itself)
#
# Everything (engine, renderer, qagame, cgame, ui) is linked into one
# executable; no renderer or game shared libraries are built or loaded.
#
# Options:
#   PLATFORM=linux     linux (default) or dc (Dreamcast: KOS toolchain, pvr
#                      renderer, no audio yet, keyboard + mouse input)
#   ARCH=x86           x86 (-m32, default) or x86_64
#   RENDERER=opengl1   opengl1 (default), rend2, or pvr (Dreamcast PowerVR
#                      backend, run on PC through tools/gpu_pvr; x86 only)
#   AUDIO=0            no sound at all: no backend, codecs (ogg/vorbis/opus)
#                      or VoIP are built; all S_* calls do nothing
#   VIDEO=0            no RoQ video player: cinematics are skipped
#   TEXTURES=0         world/model textures become one pixel of their average
#                      colour (menus, fonts and lightmaps stay); opengl1/pvr
#   MAP=<name>         start straight into this map when no + commands are
#                      given; pvr builds default to escape1, MAP= goes to
#                      the menu as normal
#   DEBUG=1            debug build (-O0 -g)
#   BASEDIR=<dir>      default fs_basepath (default: <repo>/assets)
#   V=1                show full command lines
#

PLATFORM ?= linux
ARCH     ?= x86
RENDERER ?= opengl1
DEBUG    ?= 0
AUDIO    ?= 1
TEXTURES ?= 1
VIDEO    ?= 1

ifeq ($(PLATFORM),dc)
  ifndef KOS_BASE
    $(error PLATFORM=dc needs the KOS environment: source /opt/toolchains/dc/kos/environ.sh)
  endif
  override ARCH = sh4
  override RENDERER = pvr
  override AUDIO = 0
  override VIDEO = 0
  BASEDIR ?= /cd
else ifneq ($(PLATFORM),linux)
  $(error PLATFORM must be linux or dc)
endif

BUILD_DIR ?= build
BASEDIR   ?= $(CURDIR)/assets

CC  ?= gcc
CXX ?= g++
ifeq ($(origin CC),default)
  CC = gcc
endif
ifeq ($(origin CXX),default)
  CXX = g++
endif
HOST_CC  ?= $(CC)
HOST_CXX ?= $(CXX)
OBJCOPY  ?= objcopy

ifeq ($(PLATFORM),dc)
  # kos-cc/kos-c++ add the KOS flags, includes and (when linking) libraries
  override CC  = kos-cc
  override CXX = kos-c++
  HOST_CC  = gcc
  HOST_CXX = g++
  OBJCOPY  = $(KOS_CC_PREFIX)-objcopy
  # plain gcc for the partial module links: kos-cc would add the KOS link
  # script and libraries to a -r link
  MODULE_LD = $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-gcc -ml $(KOS_SH4_PRECISION) -r -nostdlib
  # SH ELF C symbols carry a leading underscore
  SYM_PREFIX = _
endif

ifeq ($(V),1)
  Q =
  echo_cmd = @:
else
  Q = @
  echo_cmd = @echo
endif

ifeq ($(ARCH),x86)
  # non-PIE: avoids i386 PIC thunks (which break module symbol localizing)
  # and text relocations from the hand-written asm
  ARCH_FLAGS  = -m32 -fno-pie
  ARCH_LDFLAGS = -no-pie
  ARCH_STRING = i386
  ARCH_OPT    = -march=i586
else ifeq ($(ARCH),x86_64)
  ARCH_FLAGS  = -m64
  ARCH_STRING = x86_64
  ARCH_OPT    =
else ifeq ($(ARCH),sh4)
  # KOS turns on LTO; it would undo the module symbol localizing
  ARCH_FLAGS  = -fno-lto
  ARCH_STRING = sh4
  ARCH_OPT    =
else
  $(error ARCH must be x86 or x86_64)
endif

ifeq ($(PLATFORM),dc)
  # no SDL on the Dreamcast; its bundled headers still provide the GL types
  SDL_CFLAGS =
  SDL_LIBS   =
else
  SDL_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT)
  SDL_LIBS   ?= -lSDL2
endif

MODULE_LD ?= $(CC) $(ARCH_FLAGS) -r -nostdlib

.PHONY: all sp game clean pvrtest assets disc

all: sp

sp:
	@$(MAKE) --no-print-directory GAME=$@ game

clean:
	rm -rf $(BUILD_DIR)

#############################################################################
# tools/gpu_pvr: KOS PVR API on a software PVR, for running Dreamcast
# renderer code on PC (32-bit only)
#############################################################################

ifneq ($(PLATFORM),dc)
GPU_PVR_DIR = tools/gpu_pvr
GPU_PVR_OUT = $(BUILD_DIR)/gpu_pvr-$(ARCH)
include $(GPU_PVR_DIR)/gpu_pvr.mk
endif

PVRTEST = $(GPU_PVR_OUT)/pvrtest

pvrtest: $(PVRTEST)

$(PVRTEST): $(GPU_PVR_DIR)/test/pvrtest.c $(GPU_PVR_LIB)
	$(echo_cmd) "LD $@"
	$(Q)$(CXX) $(ARCH_FLAGS) $(ARCH_LDFLAGS) -O2 -w $(GPU_PVR_CFLAGS) $(SDL_CFLAGS) \
	  -x c $< -x none $(GPU_PVR_LIB) $(SDL_LIBS) -lpthread -lm -o $@

#############################################################################
# make assets: tools/rtcwconv over the models and images in the game's pk3s.
# The converted files have their own extensions (.mdsc, .dt), so they shadow
# nothing and the renderer picks them first (.dt: the pvr renderer only).
#############################################################################

ASSETS_DIR ?= $(CURDIR)/assets/main
ASSETS_OUT  = $(BUILD_DIR)/assets
KOS_BASE   ?= /opt/toolchains/dc/kos
PVRTEX     ?= $(KOS_BASE)/utils/pvrtex/pvrtex
RTCWCONV    = $(BUILD_DIR)/tools/rtcwconv
RTCWCONV_SRC = $(wildcard tools/rtcwconv/*.cpp tools/rtcwconv/TriStripper/src/*.cpp)
RTCWCONV_HDR = $(wildcard tools/rtcwconv/*.h tools/rtcwconv/TriStripper/include/*.h \
  tools/rtcwconv/TriStripper/include/detail/*.h) mdsc/mdsc.h

# the pk3s in the order the game loads them (later ones win): the sp_ paks
# first, then the shared ones; never the mp_ ones
ALL_PAKS = $(filter-out %/sp_dc.pk3,$(sort $(wildcard $(ASSETS_DIR)/*.pk3)))
SP_PAKS  = $(filter $(ASSETS_DIR)/sp_%,$(ALL_PAKS)) \
  $(filter-out $(ASSETS_DIR)/sp_% $(ASSETS_DIR)/mp_%,$(ALL_PAKS))

assets: $(ASSETS_DIR)/sp_dc.pk3

$(RTCWCONV): $(RTCWCONV_SRC) mdsc/mdsc.c $(RTCWCONV_HDR)
	$(echo_cmd) "HOST_CXX $@"
	@mkdir -p $(@D)
	$(Q)$(HOST_CXX) -std=c++17 -O2 -w -Imdsc -Itools/rtcwconv -Itools/rtcwconv/TriStripper/include \
	  $(RTCWCONV_SRC) -x c mdsc/mdsc.c -x none -lm -o $@

# 1 = sp, 2 = its pk3s in load order
define assets_pk3
$(ASSETS_DIR)/$(1)_dc.pk3: $(RTCWCONV) $(2)
	$$(echo_cmd) "ASSETS $$@"
	$$(Q)rm -rf $(ASSETS_OUT)/$(1) && mkdir -p $(ASSETS_OUT)/$(1)/src $(ASSETS_OUT)/$(1)/dc
	$$(Q)for p in $(2); do unzip -qq -o -C "$$$$p" '*.mds' '*.tga' '*.jpg' '*.bsp' -d $(ASSETS_OUT)/$(1)/src 2>/dev/null; \
	  [ $$$$? -le 11 ] || exit 1; done
	$$(Q)test -x $(PVRTEX) || { echo "no pvrtex at $(PVRTEX): set PVRTEX" >&2; exit 1; }
	$$(Q)$(RTCWCONV) -p $(PVRTEX) $(ASSETS_OUT)/$(1)/src $(ASSETS_OUT)/$(1)/dc
	$$(Q)cd $(ASSETS_OUT)/$(1)/dc && rm -f ../$(1)_dc.pk3 && zip -qr9 ../$(1)_dc.pk3 .
	$$(Q)cp $(ASSETS_OUT)/$(1)/$(1)_dc.pk3 $$@
endef
$(eval $(call assets_pk3,sp,$(SP_PAKS)))

#############################################################################
# make disc: the Dreamcast build and the sp pk3s on a selfboot .cdi; the
# data goes in main/ at the root of the disc (/cd/main on the Dreamcast)
#############################################################################

MKDCDISC  ?= mkdcdisc
DISC_DIR   = $(BUILD_DIR)/disc
DISC_CDI   = $(BUILD_DIR)/iowolfsp.cdi
DISC_PAKS  = $(SP_PAKS) $(wildcard $(ASSETS_DIR)/sp_dc.pk3)
DISC_FILES = $(wildcard $(ASSETS_DIR)/scripts/translation.cfg)

disc:
	@. $(KOS_BASE)/environ.sh && $(MAKE) --no-print-directory PLATFORM=dc sp
	@test -n "$(DISC_PAKS)" || { echo "no pk3s in $(ASSETS_DIR)" >&2; exit 1; }
	@test -f $(ASSETS_DIR)/sp_dc.pk3 || echo "warning: no sp_dc.pk3 (make assets): the disc has the PC data only" >&2
	$(echo_cmd) "DISC $(DISC_CDI)"
	$(Q)rm -rf $(DISC_DIR) && mkdir -p $(DISC_DIR)/main/scripts
	$(Q)for f in $(DISC_PAKS); do ln -f "$$f" $(DISC_DIR)/main/ 2>/dev/null || cp "$$f" $(DISC_DIR)/main/; done
	$(Q)for f in $(DISC_FILES); do cp "$$f" $(DISC_DIR)/main/scripts/; done
	$(Q)$(MKDCDISC) -e $(BUILD_DIR)/sp-sh4/iowolfsp.elf -D $(DISC_DIR) -o $(DISC_CDI) \
	  -n "Return to Castle Wolfenstein" -N

ifdef GAME

#############################################################################
# per-game setup
#############################################################################

ifeq ($(GAME),sp)
  CODE      = SP/code
  BIN       = iowolfsp
  GAMETAG   = SP
  GAME_DEFS =
else
  $(error GAME must be sp)
endif

# pvr builds boot straight into a map for testing
ifeq ($(RENDERER),pvr)
  MAP ?= escape1
endif

B   = $(BUILD_DIR)/$(GAME)-$(ARCH)
ifeq ($(PLATFORM),dc)
  EXE = $(B)/$(BIN).elf
else
  EXE = $(B)/$(BIN).$(ARCH)
endif

VERSION := 1.51d
GIT_REV := $(shell git show -s --pretty=format:%h-%ad --date=short 2>/dev/null)
ifneq ($(GIT_REV),)
  PRODUCT_VERSION = $(VERSION)-$(GAMETAG)_GIT_$(GIT_REV)
else
  PRODUCT_VERSION = $(VERSION)-$(GAMETAG)
endif

#############################################################################
# flags
#############################################################################

ZDIR  = $(CODE)/zlib-1.2.11
JPDIR = $(CODE)/jpeg-8c
OGGDIR    = $(CODE)/libogg-1.3.3
VORBISDIR = $(CODE)/libvorbis-1.3.6
OPUSDIR   = $(CODE)/opus-1.2.1
OPUSFILEDIR = $(CODE)/opusfile-0.9

ifeq ($(DEBUG),1)
  OPT = -O0 -g -DDEBUG -D_DEBUG
  FAST_MATH =
else
  OPT = -O3 $(ARCH_OPT) -DNDEBUG
  FAST_MATH = -ffast-math
endif

# shared by every object (engine, renderer and game modules)
BASE_CFLAGS = $(ARCH_FLAGS) -pipe -Wall -fno-strict-aliasing -MMD \
  -DARCH_STRING=\"$(ARCH_STRING)\" -DPRODUCT_VERSION=\"$(PRODUCT_VERSION)\" \
  -DUSE_ICON -DUSE_LOCAL_HEADERS $(GAME_DEFS) \
  -DNO_GZIP -I$(ZDIR) $(if $(filter dc,$(PLATFORM)),-DIOAPI_NO_64) \
  -DUSE_INTERNAL_JPEG -I$(JPDIR) \
  $(OPT)

# engine + renderer
CLIENT_CFLAGS = $(BASE_CFLAGS) $(FAST_MATH) $(SDL_CFLAGS) -I$(CODE)/SDL2/include \
  -DUSE_STATIC_VM -DUSE_BLOOM
ifeq ($(TEXTURES),0)
  CLIENT_CFLAGS += -DNO_TEXTURES
endif
# .dt textures from make assets go to the PVR as they are
ifeq ($(RENDERER),pvr)
  CLIENT_CFLAGS += -DUSE_PVR
endif
ifeq ($(VIDEO),0)
  CLIENT_CFLAGS += -DNO_VIDEO
endif
ifeq ($(AUDIO),0)
  CLIENT_CFLAGS += -DNO_AUDIO
else
  # opus first: opus and vorbis both have an mdct.h
  CLIENT_CFLAGS += \
    -DUSE_CODEC_OPUS -DOPUS_BUILD -DHAVE_LRINTF -DFLOATING_POINT -DFLOAT_APPROX -DUSE_ALLOCA \
    -I$(OPUSDIR)/include -I$(OPUSDIR)/celt -I$(OPUSDIR)/silk -I$(OPUSDIR)/silk/float \
    -I$(OPUSFILEDIR)/include \
    -DUSE_CODEC_VORBIS -I$(VORBISDIR)/include -I$(VORBISDIR)/lib -I$(OGGDIR)/include
endif

# game modules: hidden visibility so only dllEntry/vmMain stay global
MOD_CFLAGS = $(BASE_CFLAGS) -fvisibility=hidden

LDFLAGS += $(ARCH_FLAGS) $(ARCH_LDFLAGS)
ifeq ($(PLATFORM),dc)
  LIBS = -lm
else
  LIBS = $(SDL_LIBS) -lm -ldl -lrt -lpthread
endif

#############################################################################
# sources (paths relative to $(CODE))
#############################################################################

rel = $(patsubst $(CODE)/%,%,$(wildcard $(addprefix $(CODE)/,$(1))))

SOUND_SRC = $(call rel,client/snd_*.c) sdl/sdl_snd.c
CODEC_SRC = \
  $(call rel,libogg-1.3.3/src/*.c) \
  $(filter-out %/barkmel.c %/psytune.c %/tone.c,$(call rel,libvorbis-1.3.6/lib/*.c)) \
  $(filter-out %/opus_custom_demo.c,$(call rel,opus-1.2.1/celt/*.c)) \
  $(call rel,opus-1.2.1/silk/*.c opus-1.2.1/silk/float/*.c) \
  $(filter-out %/opus_compare.c %/opus_demo.c %/repacketizer_demo.c,$(call rel,opus-1.2.1/src/*.c)) \
  $(filter-out %/wincerts.c,$(call rel,opusfile-0.9/src/*.c))

ENGINE_SRC = \
  $(filter-out client/snd_%,$(call rel,client/*.c)) \
  $(filter-out server/sv_wallhack.c,$(call rel,server/*.c)) \
  $(filter-out qcommon/vm_armv7l.c qcommon/vm_none.c qcommon/vm_powerpc.c \
    qcommon/vm_powerpc_asm.c qcommon/vm_sparc.c qcommon/vm_x86.c,$(call rel,qcommon/*.c)) \
  sys/con_log.c sys/sys_main.c sys/sys_unix.c \
  $(filter-out splines/q_shared.cpp,$(call rel,splines/*.cpp)) \
  $(call rel,zlib-1.2.11/*.c)

ifeq ($(AUDIO),0)
  # snd_main.c is the S_* front end; with NO_AUDIO it never starts a backend
  ENGINE_SRC += client/snd_main.c
else
  ENGINE_SRC += $(SOUND_SRC) $(CODEC_SRC)
endif

ifeq ($(PLATFORM),dc)
  # no VM compiler on SH4 (all modules are static anyway)
  ENGINE_SRC += qcommon/vm_none.c sys/con_passive.c
  # maple keyboard and mouse instead of SDL input
  DC_OBJ = $(B)/dc/dc_input.c.o $(B)/dc/dc_posix.c.o
else
  ENGINE_SRC += qcommon/vm_x86.c sdl/sdl_input.c sys/con_tty.c asm/snapvector.c asm/ftola.c
  DC_OBJ =
endif
ifeq ($(ARCH),x86)
  ENGINE_SRC += asm/matha.s
  ifneq ($(AUDIO),0)
    ENGINE_SRC += asm/snd_mixa.s
  endif
endif

BOTLIB_SRC = $(call rel,botlib/*.c)

ifeq ($(RENDERER),opengl1)
  RENDERER_SRC = $(filter-out renderer/tr_subs.c,$(call rel,renderer/*.c))
  GLSL_SRC =
else ifeq ($(RENDERER),rend2)
  RENDERER_SRC = $(filter-out rend2/tr_subs.c,$(call rel,rend2/*.c))
  GLSL_SRC = $(call rel,rend2/glsl/*.glsl)
else ifeq ($(RENDERER),pvr)
  RENDERER_SRC = $(filter-out renderer/tr_subs.c,$(call rel,renderer/*.c))
  GLSL_SRC =
else
  $(error RENDERER must be opengl1, rend2 or pvr)
endif
ifeq ($(RENDERER),pvr)
  ifeq ($(filter x86 sh4,$(ARCH)),)
    $(error RENDERER=pvr needs ARCH=x86: gpu_pvr keeps host pointers in 32 bits)
  endif
  # the opengl1 renderer front end on pvr/pvr_gl.c instead of SDL + OpenGL
  RENDERER_SRC += $(call rel,jpeg-8c/*.c)
  PVR_OBJ = $(B)/pvr/pvr_gl.c.o $(B)/pvr/pvr_glimp.c.o
  ifeq ($(PLATFORM),dc)
    RENDERER_LIBS =
  else
    RENDERER_LIBS = $(GPU_PVR_LIB) -lstdc++
  endif
else
  RENDERER_SRC += sdl/sdl_gamma.c sdl/sdl_glimp.c $(call rel,jpeg-8c/*.c)
  PVR_OBJ =
  RENDERER_LIBS =
endif

BG_SRC = game/bg_animation.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c game/bg_lib.c

QAGAME_SRC = $(call rel,game/*.c)
CGAME_SRC  = $(call rel,cgame/*.c) $(BG_SRC)
UI_SRC     = $(call rel,ui/*.c) game/bg_misc.c game/bg_lib.c
MODCOMMON_SRC = qcommon/q_math.c qcommon/q_shared.c

#############################################################################
# objects
#############################################################################

obj = $(patsubst %,$(B)/$(1)/%.o,$(2))

ENGINE_OBJ   = $(call obj,engine,$(ENGINE_SRC))
BOTLIB_OBJ   = $(call obj,botlib,$(BOTLIB_SRC))
RENDERER_OBJ = $(call obj,engine,$(RENDERER_SRC))
# MDSC models (make assets); the opengl1 renderer reads them, rend2 doesn't
ifneq ($(RENDERER),rend2)
  MDSC_OBJ   = $(B)/mdsc/mdsc.c.o
endif
GLSL_OBJ     = $(patsubst %,$(B)/glsl/%.o,$(GLSL_SRC))
QAGAME_OBJ   = $(call obj,qagame,$(QAGAME_SRC))
# cgame also links the -DUI build of ui_shared, like the original Makefiles
CGAME_OBJ    = $(call obj,cgame,$(CGAME_SRC)) $(B)/ui/ui/ui_shared.c.o
UI_OBJ       = $(call obj,ui,$(UI_SRC))
MODCOMMON_OBJ = $(call obj,modcommon,$(MODCOMMON_SRC))

# each game module is pre-linked into one relocatable object
MODULE_OBJ = $(B)/qagame.o $(B)/cgame.o $(B)/ui.o

ALL_OBJ = $(ENGINE_OBJ) $(BOTLIB_OBJ) $(RENDERER_OBJ) $(GLSL_OBJ) $(PVR_OBJ) $(DC_OBJ) $(MDSC_OBJ) \
  $(QAGAME_OBJ) $(CGAME_OBJ) $(UI_OBJ) $(MODCOMMON_OBJ)

STRINGIFY = $(B)/tools/stringify

#############################################################################
# rules
#############################################################################

game: $(EXE)

$(EXE): $(ENGINE_OBJ) $(BOTLIB_OBJ) $(RENDERER_OBJ) $(GLSL_OBJ) $(PVR_OBJ) $(DC_OBJ) $(MDSC_OBJ) $(MODULE_OBJ) $(filter %.a,$(RENDERER_LIBS))
	$(echo_cmd) "LD $@"
	$(Q)$(CXX) $(LDFLAGS) -o $@ $(filter %.o,$^) $(RENDERER_LIBS) $(LIBS)

# pvr/ is built against the game's renderer
$(B)/pvr/%.c.o: pvr/%.c
	$(echo_cmd) "PVR_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CLIENT_CFLAGS) -I$(CODE)/renderer $(GPU_PVR_CFLAGS) -c $< -o $@

# mdsc/ is shared by the game and tools/rtcwconv
$(B)/mdsc/%.c.o: mdsc/%.c
	$(echo_cmd) "MDSC_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CLIENT_CFLAGS) -c $< -o $@

# dc/ is the Dreamcast platform glue
$(B)/dc/%.c.o: dc/%.c
	$(echo_cmd) "DC_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CLIENT_CFLAGS) -I$(CODE)/client -I$(CODE)/qcommon -c $< -o $@

# Combine a module's objects, make every hidden symbol local so modules
# can't collide with each other or the engine, then rename the entry points.
define link_module
$(B)/$(1).o: $$($(2)_OBJ) $$(MODCOMMON_OBJ)
	$$(echo_cmd) "MODULE $$@"
	$$(Q)$$(MODULE_LD) -o $$@.tmp $$^
	$$(Q)$$(OBJCOPY) --localize-hidden \
	  --redefine-sym $$(SYM_PREFIX)dllEntry=$$(SYM_PREFIX)$(1)_dllEntry \
	  --redefine-sym $$(SYM_PREFIX)vmMain=$$(SYM_PREFIX)$(1)_vmMain $$@.tmp $$@
	$$(Q)rm -f $$@.tmp
endef
$(eval $(call link_module,qagame,QAGAME))
$(eval $(call link_module,cgame,CGAME))
$(eval $(call link_module,ui,UI))

$(B)/engine/%.c.o: $(CODE)/%.c
	$(echo_cmd) "CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(CLIENT_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(B)/engine/%.cpp.o: $(CODE)/%.cpp
	$(echo_cmd) "CXX $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CLIENT_CFLAGS) -c $< -o $@

$(B)/engine/%.s.o: $(CODE)/%.s
	$(echo_cmd) "AS $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(BASE_CFLAGS) -x assembler-with-cpp -c $< -o $@

# default fs_basepath; game data goes in $(BASEDIR)/main
$(B)/engine/sys/sys_main.c.o: EXTRA_CFLAGS = -DDEFAULT_BASEDIR=\"$(BASEDIR)\"
$(B)/engine/sys/sys_main.c.o: Makefile

ifneq ($(MAP),)
$(B)/engine/qcommon/common.c.o: EXTRA_CFLAGS = -DAUTOMAP=\"$(MAP)\"
endif
$(B)/engine/qcommon/common.c.o: Makefile $(B)/.map
# rebuild common.c when MAP changes
ifneq ($(MAP),$(shell cat $(B)/.map 2>/dev/null))
  $(shell mkdir -p $(B) && echo '$(MAP)' > $(B)/.map)
endif
$(B)/.map: ;

# SSE helpers need a CPU with SSE on x86
ifeq ($(ARCH),x86)
$(B)/engine/asm/snapvector.c.o $(B)/engine/asm/ftola.c.o: EXTRA_CFLAGS = -march=k8
endif

$(B)/botlib/%.c.o: $(CODE)/%.c
	$(echo_cmd) "BOT_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(BASE_CFLAGS) $(FAST_MATH) -DBOTLIB -c $< -o $@

$(B)/qagame/%.c.o: $(CODE)/%.c
	$(echo_cmd) "GAME_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(MOD_CFLAGS) -DGAMEDLL -DQAGAME -c $< -o $@

$(B)/cgame/%.c.o: $(CODE)/%.c
	$(echo_cmd) "CGAME_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(MOD_CFLAGS) -DCGAMEDLL -DCGAME -c $< -o $@

$(B)/ui/%.c.o: $(CODE)/%.c
	$(echo_cmd) "UI_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(MOD_CFLAGS) -DUI -c $< -o $@

$(B)/modcommon/%.c.o: $(CODE)/%.c
	$(echo_cmd) "MOD_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(MOD_CFLAGS) -c $< -o $@

$(STRINGIFY): $(CODE)/tools/stringify.c
	@mkdir -p $(@D)
	$(Q)$(HOST_CC) -o $@ $<

$(B)/glsl/%.glsl.c: $(CODE)/%.glsl $(STRINGIFY)
	$(echo_cmd) "GLSL $<"
	@mkdir -p $(@D)
	$(Q)$(STRINGIFY) $< $@

$(B)/glsl/%.glsl.o: $(B)/glsl/%.glsl.c
	$(Q)$(CC) $(CLIENT_CFLAGS) -c $< -o $@

.SECONDARY:

# one build dir per game/arch: changing any option (RENDERER, AUDIO, VIDEO,
# TEXTURES, ...) rebuilds everything in it
FLAGS_STAMP = $(B)/.cflags
FLAGS_NOW := $(RENDERER) $(CC) $(CXX) $(CLIENT_CFLAGS) $(MOD_CFLAGS)
ifneq ($(FLAGS_NOW),$(shell cat $(FLAGS_STAMP) 2>/dev/null))
  $(shell mkdir -p $(B) && printf '%s\n' '$(subst ','\'',$(FLAGS_NOW))' > $(FLAGS_STAMP))
endif
$(ALL_OBJ): $(FLAGS_STAMP)

-include $(ALL_OBJ:.o=.d)

endif # GAME
