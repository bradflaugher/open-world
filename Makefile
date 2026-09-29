# OPEN WORLD - build & test
GBDK_HOME ?= /opt/gbdk
LCC       := $(GBDK_HOME)/bin/lcc
CC        ?= gcc
PYTHON    ?= python3
BUILD     := build
OBJ       := $(BUILD)/obj
ROM       := $(BUILD)/open-world.gb
BUILD_DATE ?= $(shell date -u +%Y.%m.%d)

CORE_SRC  := $(wildcard src/core/*.c)
GB_SRC    := $(wildcard src/gb/*.c)
GB_ASM    := $(wildcard src/gb/*.s)
ASSET_SRC := $(wildcard assets/*.txt) tools/gen_assets.py
HOST_CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror
HEADERS   := $(wildcard src/core/*.h src/gb/*.h)

# MBC5 + RAM + battery (0x1B), 1 SRAM bank, CGB-compatible, autobanked ROM
CFLAGS_GB := -Wf--max-allocs-per-node50000 -Isrc/core -Isrc/gb -I$(BUILD)
LDFLAGS_GB := -Wm-yt0x1B -Wm-yc -Wm-yn"OPENWORLD" -Wm-yoA -autobank -Wm-ya1 -Wl-j -Wm-yS
LCCFLAGS := $(LDFLAGS_GB) $(CFLAGS_GB)

GB_OBJS := $(patsubst src/core/%.c,$(OBJ)/core_%.o,$(CORE_SRC)) \
           $(patsubst src/gb/%.c,$(OBJ)/%.o,$(filter-out src/gb/assets.c,$(GB_SRC))) \
           $(OBJ)/assets.o \
           $(patsubst src/gb/%.s,$(OBJ)/%_s.o,$(GB_ASM))

.PHONY: all rom assets test test-host test-assets test-rom screenshots clean

all: rom

assets: src/gb/assets.c

src/gb/assets.c src/gb/assets.h: $(ASSET_SRC)
	$(PYTHON) tools/gen_assets.py

rom: $(ROM)

$(BUILD)/version.h: | $(BUILD)
	echo '#define BUILD_DATE "$(BUILD_DATE)"' > $@

$(OBJ)/core_%.o: src/core/%.c $(HEADERS) | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/assets.o: src/gb/assets.c src/gb/assets.h src/core/world.h | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/%.o: src/gb/%.c $(HEADERS) src/gb/assets.h $(BUILD)/version.h | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/%_s.o: src/gb/%.s | $(OBJ)
	$(LCC) -c -o $@ $<

$(ROM): $(GB_OBJS) | $(BUILD)
	$(LCC) $(LDFLAGS_GB) -o $@ $(GB_OBJS)
	@$(GBDK_HOME)/bin/romusage $(BUILD)/open-world.map -g 2>/dev/null | tail -n 12 || true

$(BUILD):
	mkdir -p $(BUILD)

$(OBJ):
	mkdir -p $(OBJ)

$(BUILD)/test_core: tests/test_core.c $(CORE_SRC) $(wildcard src/core/*.h) | $(BUILD)
	$(CC) $(HOST_CFLAGS) -Isrc/core -o $@ tests/test_core.c $(CORE_SRC)

$(BUILD)/test_sound: tests/test_sound.c src/gb/sound.c src/gb/sound.h | $(BUILD)
	$(CC) $(HOST_CFLAGS) -DHOST_TEST -Isrc/gb -Isrc/core -o $@ tests/test_sound.c src/gb/sound.c

$(BUILD)/owgen: tools/owgen.c $(CORE_SRC) $(wildcard src/core/*.h) | $(BUILD)
	$(CC) $(HOST_CFLAGS) -Isrc/core -o $@ tools/owgen.c $(CORE_SRC)

test-host: $(BUILD)/test_core $(BUILD)/test_sound $(BUILD)/owgen
	$(BUILD)/test_core
	$(BUILD)/test_sound

test-assets:
	$(PYTHON) -m unittest discover -s tests -p 'test_assets.py'

test-rom: $(ROM) $(BUILD)/owgen
	$(PYTHON) -m unittest discover -s tests -p 'test_rom.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_core_rom.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_playthrough.py' -v

screenshots: $(ROM) $(BUILD)/owgen
	$(PYTHON) tools/screenshots.py

test: test-host test-assets test-rom

clean:
	rm -rf $(BUILD)
