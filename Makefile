# Morrowind PSP Demake - PSP runtime build (PSPSDK) + host/tool test targets.
#
#   make              build EBOOT.PBP (needs psp-config / psp-gcc on PATH)
#   make host-test    build and run the portable runtime tests natively
#   make tools-test   run the demake cooker unit tests (python3 + Pillow)
#   make test         host-test + tools-test
#   make clean

TARGET    = pspwind
OBJS      = src/main.o src/mem.o src/vfpu.o src/selftest.o

INCDIR    = src
CFLAGS    = -std=c99 -O2 -G0 -Wall -Wextra -Werror -fno-strict-aliasing
ASFLAGS   = $(CFLAGS)
LIBDIR    =
LDFLAGS   =
LIBS      = -lm

BUILD_PRX = 1
PSP_FW_VERSION = 661

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Morrowind PSP Demake

# host-test / tools-test / test work without the PSP toolchain installed.
HOST_GOALS := host-test tools-test test clean-host
PSP_GOALS  := $(filter-out $(HOST_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all))

ifneq ($(PSP_GOALS),)
PSPSDK := $(shell psp-config --pspsdk-path 2>/dev/null)
ifeq ($(PSPSDK),)
$(error psp-config not found on PATH - install the pspdev toolchain (see README.md))
endif
include $(PSPSDK)/lib/build.mak
clean: clean-host
endif

.PHONY: host-test tools-test test clean-host
host-test:
	$(MAKE) -C tests/host
tools-test:
	cd tools/demake_cooker && python3 -m unittest discover -s tests
test: host-test tools-test
clean-host:
	$(MAKE) -C tests/host clean
