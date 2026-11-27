# File Manager — Retro-Go SD GWHB homebrew for the Game & Watch.
#
#   make                         build + pack FileManager.bin  (-> SD /homebrews/)
#   make GCC_PATH=/path/to/bin   toolchain not on PATH (Arm GNU Toolchain 15.2.rel1)
#   make docker                  same toolchain image as retro-go-sd CI

PROJECT_KIND ?= homebrew
ifneq ($(PROJECT_KIND),homebrew)
$(error File Manager is a homebrew: PROJECT_KIND=homebrew)
endif

CORE_NAME  := filemanager
CORE_ENTRY := app_main

CORE_C_SOURCES := \
src/main.c \
src/ovl.c \
src/filetypes.c \
src/fsops.c \
src/osk.c \
src/viewer.c \
src/editor.c \
src/kbd.c \
src/image.c \
src/video.c \
src/audio.c \
src/flac.c \
src/ogg.c \
src/third_party/picojpeg/picojpeg.c \
src/third_party/miniz/miniz_tinfl.c \
src/tools.c \
src/hash.c \
src/extract.c \
src/unzip.c \
src/tinfl_tools.c

CORE_C_INCLUDES := -Isrc
CORE_C_DEFS := -DPROJECT_KIND_HOMEBREW=1 -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

GNW_CORE_SDK ?= sdk

# Viewers and players are overlays loaded on demand (ld/filemanager.ld, src/ovl.c)
CORE_LDSCRIPT := ld/filemanager.ld
FM_OVERLAYS := text image media tools
BUILD_DIR ?= build

PACKED_BIN := FileManager.bin
HB_NAME    := File Manager
COVER_SRC  := src/assets/cover.png
COVER_JPG  := $(BUILD_DIR)/cover.jpg

include $(GNW_CORE_SDK)/Makefile

CFLAGS += -std=gnu11 -Wno-format-truncation

CORE_VERSION ?= 1.2.0

.PHONY: pack cover
cover: $(COVER_JPG)
$(COVER_JPG): $(COVER_SRC)
	$(V)$(ECHO) [ COVER ] $(COVER_JPG)
	$(V)mkdir -p $(BUILD_DIR)
	$(V)python3 -c "from pathlib import Path; from PIL import Image; \
img=Image.open('$(COVER_SRC)').convert('RGB'); \
img.thumbnail((186,100)); \
img.save('$(COVER_JPG)', 'JPEG', quality=85, optimize=True); \
sz=Path('$(COVER_JPG)').stat().st_size; \
assert sz <= 10*1024, f'cover too big: {sz}'"

PACK_HOMEBREW := $(GNW_CORE_SDK)/tools/pack_homebrew.py
# The file manager has no Retro-Go save states (no load/save handlers): GWHB_FLAG_NO_SAVESTATES
# (1 << 0, gwhb.h) tells the launcher to hide Resume / Save / Reload for it. Older firmware
# ignores unknown flag bits.
GWHB_FLAGS := 0x1
# Each overlay out of the ELF (code + data; the packer drops the trailing BSS)
$(BUILD_DIR)/ovl_%.bin: $(TARGET_ELF)
	$(V)$(CP) -O binary --only-section=.fm_ovl_$* $< $@

pack: $(TARGET_BIN) $(addprefix $(BUILD_DIR)/ovl_,$(addsuffix .bin,$(FM_OVERLAYS))) $(if $(wildcard $(COVER_SRC)),$(COVER_JPG))
	$(V)$(ECHO) [ PACK GWHB ] "$(PACKED_BIN)" version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--nm $(if $(GCC_PATH),$(GCC_PATH)/)arm-none-eabi-nm \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		--flags $(GWHB_FLAGS) \
		$(if $(wildcard $(COVER_SRC)),--cover $(COVER_JPG)) \
		--out "$(BUILD_DIR)/$(PACKED_BIN)"
	$(V)python3 tools/append_overlays.py \
		--nm $(if $(GCC_PATH),$(GCC_PATH)/)arm-none-eabi-nm \
		--elf $(TARGET_ELF) --bin "$(BUILD_DIR)/$(PACKED_BIN)" \
		$(foreach o,$(FM_OVERLAYS),$(o)=$(BUILD_DIR)/ovl_$(o).bin)

all: pack

clean::
	$(V)rm -f "$(BUILD_DIR)/$(PACKED_BIN)" $(COVER_JPG)

.PHONY: docker
DOCKER_IMAGE ?= sylverb/retro-go-sd-builder:v1.5
docker:
	docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR):/opt/workdir" -w /opt/workdir \
		$(DOCKER_IMAGE) make --no-print-directory -j$$(nproc)
