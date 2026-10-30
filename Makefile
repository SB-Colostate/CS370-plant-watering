# CS370 plant watering: C17 core. Run on the Pi (gcc 14, aarch64).
#   make        build daemons and CLI into build/bin/
#   make test   build and run unit + integration tests
#   make asan   same tests built with UBSan and run under valgrind memcheck, in build-asan/
#
# Why not -fsanitize=address: the stock Pi kernel is CONFIG_ARM64_VA_BITS=39, and GCC 14's
# ASan runtime needs its allocator at 0x500000000000, which a 39-bit address space can't map
# (fails at startup with "sanitizer_allocator_primary64.h:131"). Valgrind covers the same heap
# errors (overflow, use-after-free, leaks) without depending on the VA size.

CC     = gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE -Isrc $(EXTRA_CFLAGS)
BUILD ?= build
BIN    = $(BUILD)/bin
OBJ    = $(BUILD)/obj

UBSAN_FLAGS = -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
VALGRIND    = valgrind --quiet --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect

# Prefix for running each test binary; `make asan` sets it to $(VALGRIND).
RUNNER ?=

COMMON_OBJS = $(OBJ)/src/common/paths.o
STORE_OBJS  = $(OBJ)/src/store/record.o $(OBJ)/src/store/store.o

PROGS   = $(BIN)/plantctl $(BIN)/sampler $(BIN)/hub
TESTS   = $(BIN)/test_smoke $(BIN)/test_record $(BIN)/test_store $(BIN)/test_parse
HELPERS = $(BIN)/mklog
SCRIPTS = tests/it_plantctl.sh tests/it_pipeline.sh

.PHONY: all test asan clean

all: $(PROGS)

# Depend on the Makefile so a flag change rebuilds everything.
$(OBJ)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Every binary links its listed objects; each one just declares its prerequisites below.
$(BIN)/%:
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/test_smoke:  $(OBJ)/tests/test_smoke.o
$(BIN)/test_record: $(OBJ)/tests/test_record.o $(STORE_OBJS)
$(BIN)/test_store:  $(OBJ)/tests/test_store.o $(STORE_OBJS)
$(BIN)/mklog:       $(OBJ)/tests/mklog.o $(STORE_OBJS)
$(BIN)/plantctl:    $(OBJ)/src/plantctl/plantctl.o $(STORE_OBJS) $(COMMON_OBJS)
$(BIN)/sampler:     $(OBJ)/src/sampler/sampler.o $(OBJ)/src/sampler/parse.o $(COMMON_OBJS)
$(BIN)/hub:         $(OBJ)/src/hub/hub.o $(STORE_OBJS) $(COMMON_OBJS)
$(BIN)/test_parse:  $(OBJ)/tests/test_parse.o $(OBJ)/src/sampler/parse.o

test: all $(TESTS) $(HELPERS)
	@for t in $(TESTS); do echo "== $(RUNNER) $$t"; $(RUNNER) $$t || exit 1; done
	@for s in $(SCRIPTS); do echo "== $$s"; BIN=$(BIN) RUNNER="$(RUNNER)" sh $$s || exit 1; done
	@echo "ALL TESTS PASSED ($(BUILD))"

asan:
	@command -v valgrind >/dev/null || { echo "valgrind not installed: sudo apt install valgrind"; exit 1; }
	$(MAKE) BUILD=build-asan EXTRA_CFLAGS="$(UBSAN_FLAGS)" RUNNER="$(VALGRIND)" test

clean:
	rm -rf build build-asan

-include $(wildcard $(OBJ)/*/*.d $(OBJ)/*/*/*.d)
