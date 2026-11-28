# Host-side tests for the portable runtime code (scalar math paths).
#   make            allocator + math self-tests
#   make world      cook the synthetic test world and load every zone
HOSTCC ?= cc
PYTHON ?= python3
SRC_DIR := ../../src
COOKER := ../../tools/demake_cooker
BUILD := ../../build/host
CFLAGS := -std=c99 -O1 -Wall -Wextra -Werror -pedantic -I$(SRC_DIR) -fsanitize=address,undefined -g
LDLIBS := -lm

CORE := $(SRC_DIR)/mem.c $(SRC_DIR)/vfpu.c
HDRS := $(wildcard $(SRC_DIR)/*.h)

.PHONY: all run world clean
all: run

host_tests: test_main.c $(CORE) $(SRC_DIR)/selftest.c $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ test_main.c $(CORE) $(SRC_DIR)/selftest.c $(LDLIBS)

WORLD_SRC := $(SRC_DIR)/pak.c $(SRC_DIR)/texcache.c $(SRC_DIR)/zone.c $(SRC_DIR)/anim.c
RPG_ONLY := $(SRC_DIR)/inflate.c $(SRC_DIR)/mwfuncs.c $(SRC_DIR)/rpgdb.c $(SRC_DIR)/vm.c $(SRC_DIR)/dialogue.c \
	$(SRC_DIR)/rpg.c $(SRC_DIR)/rpg_items.c $(SRC_DIR)/rpg_trade.c
RPG_SRC := $(SRC_DIR)/pak.c $(RPG_ONLY)
GAME_SRC := $(WORLD_SRC) $(SRC_DIR)/actor.c $(SRC_DIR)/phys.c $(SRC_DIR)/game.c $(SRC_DIR)/ai.c $(SRC_DIR)/audio.c \
	$(SRC_DIR)/save.c $(RPG_ONLY)
UI_SRC := $(SRC_DIR)/ui.c $(SRC_DIR)/ui_menu.c $(SRC_DIR)/ui_trade.c $(SRC_DIR)/ui_char.c

world_test: world_test.c $(CORE) $(WORLD_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ world_test.c $(CORE) $(WORLD_SRC) $(LDLIBS)

game_test: game_test.c $(CORE) $(GAME_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ game_test.c $(CORE) $(GAME_SRC) $(LDLIBS)

ui_test: ui_test.c $(CORE) $(GAME_SRC) $(UI_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ ui_test.c $(CORE) $(GAME_SRC) $(UI_SRC) $(LDLIBS)

save_test: save_test.c $(CORE) $(GAME_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ save_test.c $(CORE) $(GAME_SRC) $(LDLIBS)

persist_test: persist_test.c $(CORE) $(GAME_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ persist_test.c $(CORE) $(GAME_SRC) $(LDLIBS)

ai_test: ai_test.c $(CORE) $(GAME_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ ai_test.c $(CORE) $(GAME_SRC) $(LDLIBS)

audio_test: audio_test.c $(CORE) $(RPG_SRC) $(SRC_DIR)/audio.c $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ audio_test.c $(CORE) $(RPG_SRC) $(SRC_DIR)/audio.c $(LDLIBS)

mech_test: mech_test.c $(CORE) $(RPG_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ mech_test.c $(CORE) $(RPG_SRC) $(LDLIBS)

rpg_test: rpg_test.c $(CORE) $(RPG_SRC) $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ rpg_test.c $(CORE) $(RPG_SRC) $(LDLIBS)

run: host_tests
	./host_tests

$(BUILD)/testworld/data/zones.dpak: $(wildcard $(COOKER)/*.py)
	rm -rf $(BUILD)/testworld
	$(PYTHON) $(COOKER)/make_testworld.py --out $(BUILD)/testworld
	$(PYTHON) $(COOKER)/cook_all.py --data "$(BUILD)/testworld/Data Files" --out $(BUILD)/testworld/data --jobs 2

world: world_test game_test rpg_test mech_test audio_test ai_test persist_test save_test ui_test $(BUILD)/testworld/data/zones.dpak
	./world_test $(BUILD)/testworld/data --expect-testworld
	./rpg_test $(BUILD)/testworld/data
	./mech_test $(BUILD)/testworld/data
	./audio_test $(BUILD)/testworld/data
	./ai_test $(BUILD)/testworld/data
	./persist_test $(BUILD)/testworld/data $(BUILD)
	./save_test $(BUILD)/testworld/data $(BUILD)
	mkdir -p $(BUILD)/screens
	./ui_test $(BUILD)/testworld/data $(BUILD)/screens
	./game_test $(BUILD)/testworld/data

clean:
	rm -f host_tests world_test game_test rpg_test mech_test audio_test ai_test persist_test save_test ui_test
