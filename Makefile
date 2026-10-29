# CS370 plant watering: C17 core. Run on the Pi (gcc 14, aarch64).
#   make        build daemons and CLI into build/bin/
#   make test   build and run unit + integration tests
#   make asan   same tests under AddressSanitizer + UBSan, in build-asan/

CC     = gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE -Isrc $(EXTRA_CFLAGS)
BUILD ?= build
BIN    = $(BUILD)/bin
OBJ    = $(BUILD)/obj

ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

PROGS =
TESTS = $(BIN)/test_smoke

.PHONY: all test asan clean

all: $(PROGS)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN)/test_smoke: $(OBJ)/tests/test_smoke.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

test: all $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done
	@echo "ALL TESTS PASSED ($(BUILD))"

asan:
	$(MAKE) BUILD=build-asan EXTRA_CFLAGS="$(ASAN_FLAGS)" test

clean:
	rm -rf build build-asan

-include $(wildcard $(OBJ)/*/*.d $(OBJ)/*/*/*.d)
