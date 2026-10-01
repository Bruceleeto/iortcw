#
# iortcw - single top-level Makefile (Linux)
#
#   make sp            build single player  -> build/sp-x86/iowolfsp.x86
#   make mp            build multiplayer    -> build/mp-x86/iowolfmp.x86
#   make               build both
#   make clean         remove build/
#
# Everything (engine, renderer, qagame, cgame, ui) is linked into one
# executable; no renderer or game shared libraries are built or loaded.
#
# Options:
#   ARCH=x86           x86 (-m32, default) or x86_64
#   RENDERER=opengl1   opengl1 (default) or rend2
#   DEBUG=1            debug build (-O0 -g)
#   BASEDIR=<dir>      default fs_basepath (default: <repo>/assets)
#   V=1                show full command lines
#

ARCH     ?= x86
RENDERER ?= opengl1
DEBUG    ?= 0

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
OBJCOPY  ?= objcopy

ifeq ($(V),1)
  Q =
  echo_cmd = @:
else
  Q = @
  echo_cmd = @echo
endif

.PHONY: all sp mp game clean

all: sp mp

sp mp:
	@$(MAKE) --no-print-directory GAME=$@ game

clean:
	rm -rf $(BUILD_DIR)

ifdef GAME

#############################################################################
# per-game setup
#############################################################################

ifeq ($(GAME),sp)
  CODE      = SP/code
  BIN       = iowolfsp
  GAMETAG   = SP
  GAME_DEFS =
else ifeq ($(GAME),mp)
  CODE      = MP/code
  BIN       = iowolfmp
  GAMETAG   = MP
  GAME_DEFS = -DUSE_PBMD5
else
  $(error GAME must be sp or mp)
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
else
  $(error ARCH must be x86 or x86_64)
endif

B   = $(BUILD_DIR)/$(GAME)-$(ARCH)
EXE = $(B)/$(BIN).$(ARCH)

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
FTDIR = $(CODE)/freetype-2.9
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
  -DNO_GZIP -I$(ZDIR) \
  -DUSE_INTERNAL_JPEG -I$(JPDIR) \
  -DBUILD_FREETYPE -DFT2_BUILD_LIBRARY -I$(FTDIR)/include \
  $(OPT)

SDL_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT)
SDL_LIBS   ?= -lSDL2

# engine + renderer
CLIENT_CFLAGS = $(BASE_CFLAGS) $(FAST_MATH) $(SDL_CFLAGS) -I$(CODE)/SDL2/include \
  -DUSE_STATIC_VM -DUSE_BLOOM -DUSE_MUMBLE -DUSE_VOIP \
  -DUSE_CODEC_OPUS -DOPUS_BUILD -DHAVE_LRINTF -DFLOATING_POINT -DFLOAT_APPROX -DUSE_ALLOCA \
  -I$(OPUSDIR)/include -I$(OPUSDIR)/celt -I$(OPUSDIR)/silk -I$(OPUSDIR)/silk/float \
  -I$(OPUSFILEDIR)/include \
  -DUSE_CODEC_VORBIS -I$(VORBISDIR)/include -I$(VORBISDIR)/lib -I$(OGGDIR)/include

# game modules: hidden visibility so only dllEntry/vmMain stay global
MOD_CFLAGS = $(BASE_CFLAGS) -fvisibility=hidden

LDFLAGS += $(ARCH_FLAGS) $(ARCH_LDFLAGS)
LIBS = $(SDL_LIBS) -lm -ldl -lrt -lpthread

#############################################################################
# sources (paths relative to $(CODE))
#############################################################################

rel = $(patsubst $(CODE)/%,%,$(wildcard $(addprefix $(CODE)/,$(1))))

ENGINE_SRC = \
  $(call rel,client/*.c) \
  $(filter-out server/sv_wallhack.c,$(call rel,server/*.c)) \
  $(filter-out qcommon/vm_armv7l.c qcommon/vm_none.c qcommon/vm_powerpc.c \
    qcommon/vm_powerpc_asm.c qcommon/vm_sparc.c,$(call rel,qcommon/*.c)) \
  sys/con_log.c sys/con_tty.c sys/sys_main.c sys/sys_unix.c \
  sdl/sdl_input.c sdl/sdl_snd.c \
  $(filter-out splines/q_shared.cpp,$(call rel,splines/*.cpp)) \
  $(call rel,zlib-1.2.11/*.c) \
  $(call rel,libogg-1.3.3/src/*.c) \
  $(filter-out %/barkmel.c %/psytune.c %/tone.c,$(call rel,libvorbis-1.3.6/lib/*.c)) \
  $(filter-out %/opus_custom_demo.c,$(call rel,opus-1.2.1/celt/*.c)) \
  $(call rel,opus-1.2.1/silk/*.c opus-1.2.1/silk/float/*.c) \
  $(filter-out %/opus_compare.c %/opus_demo.c %/repacketizer_demo.c,$(call rel,opus-1.2.1/src/*.c)) \
  $(filter-out %/wincerts.c,$(call rel,opusfile-0.9/src/*.c))

ifeq ($(ARCH),x86)
  ENGINE_SRC += asm/snd_mixa.s asm/matha.s asm/snapvector.c asm/ftola.c
else
  ENGINE_SRC += asm/snapvector.c asm/ftola.c
endif

BOTLIB_SRC = $(call rel,botlib/*.c)

FT_SRC = $(addprefix freetype-2.9/src/, \
  base/ftsystem.c base/ftdebug.c base/ftinit.c base/ftbase.c base/ftbbox.c \
  base/ftbdf.c base/ftbitmap.c base/ftcid.c base/ftfntfmt.c base/ftfstype.c \
  base/ftgasp.c base/ftglyph.c base/ftgxval.c base/ftlcdfil.c base/ftmm.c \
  base/ftotval.c base/ftpatent.c base/ftpfr.c base/ftstroke.c base/ftsynth.c \
  base/fttype1.c base/ftwinfnt.c truetype/truetype.c type1/type1.c cff/cff.c \
  cid/type1cid.c pfr/pfr.c type42/type42.c winfonts/winfnt.c pcf/pcf.c \
  bdf/bdf.c sfnt/sfnt.c autofit/autofit.c pshinter/pshinter.c raster/raster.c \
  smooth/smooth.c cache/ftcache.c gzip/ftgzip.c lzw/ftlzw.c bzip2/ftbzip2.c \
  psaux/psaux.c psnames/psnames.c)

ifeq ($(RENDERER),opengl1)
  RENDERER_SRC = $(filter-out renderer/tr_subs.c,$(call rel,renderer/*.c))
  GLSL_SRC =
else ifeq ($(RENDERER),rend2)
  RENDERER_SRC = $(filter-out rend2/tr_subs.c,$(call rel,rend2/*.c))
  GLSL_SRC = $(call rel,rend2/glsl/*.glsl)
else
  $(error RENDERER must be opengl1 or rend2)
endif
RENDERER_SRC += sdl/sdl_gamma.c sdl/sdl_glimp.c $(call rel,jpeg-8c/*.c) $(FT_SRC)

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
GLSL_OBJ     = $(patsubst %,$(B)/glsl/%.o,$(GLSL_SRC))
QAGAME_OBJ   = $(call obj,qagame,$(QAGAME_SRC))
# cgame also links the -DUI build of ui_shared, like the original Makefiles
CGAME_OBJ    = $(call obj,cgame,$(CGAME_SRC)) $(B)/ui/ui/ui_shared.c.o
UI_OBJ       = $(call obj,ui,$(UI_SRC))
MODCOMMON_OBJ = $(call obj,modcommon,$(MODCOMMON_SRC))

# each game module is pre-linked into one relocatable object
MODULE_OBJ = $(B)/qagame.o $(B)/cgame.o $(B)/ui.o

ALL_OBJ = $(ENGINE_OBJ) $(BOTLIB_OBJ) $(RENDERER_OBJ) $(GLSL_OBJ) \
  $(QAGAME_OBJ) $(CGAME_OBJ) $(UI_OBJ) $(MODCOMMON_OBJ)

STRINGIFY = $(B)/tools/stringify

#############################################################################
# rules
#############################################################################

game: $(EXE)

$(EXE): $(ENGINE_OBJ) $(BOTLIB_OBJ) $(RENDERER_OBJ) $(GLSL_OBJ) $(MODULE_OBJ)
	$(echo_cmd) "LD $@"
	$(Q)$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# Combine a module's objects, make every hidden symbol local so modules
# can't collide with each other or the engine, then rename the entry points.
define link_module
$(B)/$(1).o: $$($(2)_OBJ) $$(MODCOMMON_OBJ)
	$$(echo_cmd) "MODULE $$@"
	$$(Q)$$(CC) $$(ARCH_FLAGS) -r -nostdlib -o $$@.tmp $$^
	$$(Q)$$(OBJCOPY) --localize-hidden \
	  --redefine-sym dllEntry=$(1)_dllEntry \
	  --redefine-sym vmMain=$(1)_vmMain $$@.tmp $$@
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

-include $(ALL_OBJ:.o=.d)

endif # GAME
