# OPEN WORLD - build & test
GBDK_HOME ?= /opt/gbdk
LCC       := $(GBDK_HOME)/bin/lcc
CC        ?= gcc
PYTHON    ?= python3
BUILD     := build
ROM       := $(BUILD)/open-world.gb
BUILD_DATE ?= $(shell date -u +%Y.%m.%d)

CORE_SRC  := $(wildcard src/core/*.c)
GB_SRC    := $(wildcard src/gb/*.c)
GB_ASM    := $(wildcard src/gb/*.s)
ASSET_SRC := $(wildcard assets/*.txt) tools/gen_assets.py
HOST_CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror

# MBC5 + RAM + battery (0x1B), 1 SRAM bank, CGB-compatible, autobanked ROM
LCCFLAGS := -Wm-yt0x1B -Wm-yc -Wm-yn"OPENWORLD" -Wm-yoA -autobank -Wm-ya1 \
            -Wl-j -Wm-yS -Wf--max-allocs-per-node50000 -Isrc/core -I$(BUILD)

.PHONY: all rom assets test test-host test-assets test-rom screenshots clean

all: rom

assets:
	$(PYTHON) tools/gen_assets.py

rom: $(ROM)

$(ROM): $(CORE_SRC) $(GB_SRC) $(GB_ASM) $(wildcard src/core/*.h src/gb/*.h) $(ASSET_SRC) | $(BUILD)
	$(PYTHON) tools/gen_assets.py
	echo '#define BUILD_DATE "$(BUILD_DATE)"' > $(BUILD)/version.h
	$(LCC) $(LCCFLAGS) -o $@ $(CORE_SRC) $(GB_SRC) $(GB_ASM)
	@$(GBDK_HOME)/bin/romusage $(BUILD)/open-world.map -g 2>/dev/null | tail -n 12 || true

$(BUILD):
	mkdir -p $(BUILD)

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

screenshots: $(ROM)
	$(PYTHON) tools/screenshots.py

test: test-host test-assets test-rom

clean:
	rm -rf $(BUILD)
