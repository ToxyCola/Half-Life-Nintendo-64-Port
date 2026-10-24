# HL64 - Half-Life for Nintendo 64
# Requires: libdragon (preview branch) + tiny3d.  See README.md.
BUILD_DIR = build
SOURCE_DIR = src
T3D_INST ?= $(HOME)/tiny3d
include $(N64_INST)/include/n64.mk
include $(T3D_INST)/t3d.mk

N64_ROM_TITLE = "HALF-LIFE 64"
N64_ROM_SAVETYPE = sram256k
N64_ROM_REGIONFREE = true

VALVE = valve_sdk
VALVE_INC = -I$(VALVE)/common -I$(VALVE)/engine -I$(VALVE)/pm_shared -I$(VALVE)/public
VALVE_DEFS = -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp -DMAX_PHYSENTS=128 -DNDEBUG

N64_CFLAGS += -std=gnu2x -O2 -ffast-math -fno-strict-aliasing -Wno-stringop-truncation -Wno-unused-but-set-variable -Wno-unused-variable -Wno-format-truncation $(EXTRA_CFLAGS)

SRC = src/main.c src/bsp.c src/render.c src/entities.c src/sound.c src/studio.c src/npc.c src/nav.c src/player.c src/weapons.c src/skill.c src/font.c src/hud.c src/menu.c src/heapdbg.c src/lights.c
VALVE_SRC = $(VALVE)/pm_shared/pm_shared.c $(VALVE)/pm_shared/pm_math.c

OBJS = $(SRC:src/%.c=$(BUILD_DIR)/%.o) $(BUILD_DIR)/pmove.o \
       $(BUILD_DIR)/valve/pm_shared.o $(BUILD_DIR)/valve/pm_math.o

LDFLAGS += $(EXTRA_LDFLAGS)
all: hl64.z64

# Valve's movement code: compiled unmodified, warnings off (MSVC-era code)
$(BUILD_DIR)/valve/%.o: $(VALVE)/pm_shared/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC valve] $<"
	$(CC) -c $(CFLAGS) -std=gnu11 -w -Wno-error -Wno-incompatible-pointer-types $(VALVE_INC) $(VALVE_DEFS) -o $@ $<

# engine glue sees Valve's headers
$(BUILD_DIR)/pmove.o: src/pmove.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -std=gnu11 -Wno-error -Wno-incompatible-pointer-types $(VALVE_INC) $(VALVE_DEFS) -Isrc -o $@ $<

N64_MKDFS_ROOT ?= filesystem
FS_FILES = $(shell find $(N64_MKDFS_ROOT) -type f 2>/dev/null)
$(BUILD_DIR)/hl64.dfs: $(FS_FILES)
$(BUILD_DIR)/hl64.elf: $(OBJS)

hl64.z64: N64_ROM_TITLE = "HALF-LIFE 64"
hl64.z64: $(BUILD_DIR)/hl64.dfs

clean:
	rm -rf $(BUILD_DIR) hl64.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
