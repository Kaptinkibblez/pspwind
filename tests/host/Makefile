# Host-side unit tests for the portable runtime code (scalar math paths).
HOSTCC ?= cc
SRC_DIR := ../../src
CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror -pedantic -I$(SRC_DIR) -fsanitize=address,undefined -g
LDLIBS := -lm

SRCS := test_main.c $(SRC_DIR)/mem.c $(SRC_DIR)/vfpu.c $(SRC_DIR)/selftest.c
BIN := host_tests

.PHONY: all run clean
all: run

$(BIN): $(SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(HOSTCC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
