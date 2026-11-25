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

world_test: world_test.c $(CORE) $(SRC_DIR)/pak.c $(SRC_DIR)/texcache.c $(SRC_DIR)/zone.c $(HDRS)
	$(HOSTCC) $(CFLAGS) -o $@ world_test.c $(CORE) $(SRC_DIR)/pak.c $(SRC_DIR)/texcache.c $(SRC_DIR)/zone.c $(LDLIBS)

run: host_tests
	./host_tests

$(BUILD)/testworld/data/zones.dpak: $(wildcard $(COOKER)/*.py)
	rm -rf $(BUILD)/testworld
	$(PYTHON) $(COOKER)/make_testworld.py --out $(BUILD)/testworld
	$(PYTHON) $(COOKER)/cook_all.py --data "$(BUILD)/testworld/Data Files" --out $(BUILD)/testworld/data --jobs 2

world: world_test $(BUILD)/testworld/data/zones.dpak
	./world_test $(BUILD)/testworld/data --expect-testworld

clean:
	rm -f host_tests world_test
