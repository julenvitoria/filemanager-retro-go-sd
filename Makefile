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
src/fsops.c \
src/osk.c \
src/viewer.c \
src/editor.c \
src/kbd.c \
src/image.c \
src/third_party/picojpeg/picojpeg.c \
src/third_party/miniz/miniz_tinfl.c

CORE_C_INCLUDES := -Isrc
CORE_C_DEFS := -DPROJECT_KIND_HOMEBREW=1 -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build

PACKED_BIN := FileManager.bin
HB_NAME    := File Manager
COVER_SRC  := src/assets/cover.png
COVER_JPG  := $(BUILD_DIR)/cover.jpg

include $(GNW_CORE_SDK)/Makefile

CFLAGS += -std=gnu11 -Wno-format-truncation

CORE_VERSION ?= 0.1.0

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
pack: $(TARGET_BIN) $(if $(wildcard $(COVER_SRC)),$(COVER_JPG))
	$(V)$(ECHO) [ PACK GWHB ] "$(PACKED_BIN)" version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--nm $(if $(GCC_PATH),$(GCC_PATH)/)arm-none-eabi-nm \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		$(if $(wildcard $(COVER_SRC)),--cover $(COVER_JPG)) \
		--out "$(BUILD_DIR)/$(PACKED_BIN)"

all: pack

clean::
	$(V)rm -f "$(BUILD_DIR)/$(PACKED_BIN)" $(COVER_JPG)

.PHONY: docker
DOCKER_IMAGE ?= sylverb/retro-go-sd-builder:v1.5
docker:
	docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR):/opt/workdir" -w /opt/workdir \
		$(DOCKER_IMAGE) make --no-print-directory -j$$(nproc)
