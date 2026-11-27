# Morrowind PSP Demake - PSP runtime build (PSPSDK) + host/tool test targets.
#
#   make              build EBOOT.PBP (needs psp-config / psp-gcc on PATH)
#   make host-test    build and run the portable runtime tests natively
#   make tools-test   run the demake cooker unit tests (python3 + Pillow)
#   make world-test   cook the synthetic test world and load every zone natively
#   make test         host-test + tools-test + world-test
#   make package      build/psp/pspwind/ = EBOOT.PBP + cooked test world, ready
#                     to copy to ms0:/PSP/GAME/ or open in PPSSPP
#   make clean

TARGET    = pspwind
OBJS      = src/main.o src/mem.o src/vfpu.o src/selftest.o src/pak.o src/texcache.o \
            src/zone.o src/gfx.o src/anim.o src/actor.o src/phys.o src/game.o src/inflate.o src/mwfuncs.o src/rpgdb.o src/vm.o src/dialogue.o \
            src/rpg.o src/rpg_items.o src/rpg_trade.o src/ai.o src/audio.o src/save.o src/ui.o src/ui_menu.o src/ui_trade.o src/ui_char.o src/ui_gu.o

INCDIR    = src
CFLAGS    = -std=c99 -O2 -G0 -Wall -Wextra -Werror -fno-strict-aliasing
ASFLAGS   = $(CFLAGS)
LIBDIR    =
LDFLAGS   =
LIBS      = -lpspgu -lpspaudio -lpspmp3 -lpsputility -lpsppower -lm

BUILD_PRX = 1
PSP_FW_VERSION = 661

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Morrowind PSP Demake

# host-test / tools-test / test work without the PSP toolchain installed.
HOST_GOALS := host-test tools-test world-test test clean-host
PSP_GOALS  := $(filter-out $(HOST_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all))

ifneq ($(PSP_GOALS),)
PSPSDK := $(shell psp-config --pspsdk-path 2>/dev/null)
ifeq ($(PSPSDK),)
$(error psp-config not found on PATH - install the pspdev toolchain (see README.md))
endif
include $(PSPSDK)/lib/build.mak
clean: clean-host
endif

.PHONY: host-test tools-test world-test test clean-host package
host-test:
	$(MAKE) -C tests/host
tools-test:
	cd tools/demake_cooker && python3 -m unittest discover -s tests
world-test:
	$(MAKE) -C tests/host world
test: host-test tools-test world-test
clean-host:
	$(MAKE) -C tests/host clean

PACKAGE_DIR := build/psp/pspwind
package: EBOOT.PBP world-test
	rm -rf $(PACKAGE_DIR)
	mkdir -p $(PACKAGE_DIR)/data
	cp EBOOT.PBP $(PACKAGE_DIR)/
	cp build/host/testworld/data/*.dpak $(PACKAGE_DIR)/data/
	@echo "packaged -> $(PACKAGE_DIR) (copy the folder to ms0:/PSP/GAME/)"
