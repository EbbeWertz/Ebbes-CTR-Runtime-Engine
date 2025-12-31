# Variables
BUILD_DIR       := build
OUTPUT_DIR      := bin
ROMFS_DIR		:= resources/romfs
CMAKE_BUILD_DIR := $(BUILD_DIR)/cmake-build
CMAKE_TOOLCHAIN := $(DEVKITPRO)/cmake/3DS.cmake
CITRA_PATH		:= "/mnt/c/Program Files/citra/citra-qt.exe"
MAKEROM_PATH	:= "/mnt/c/devkitPro/tools/bin/makerom.exe"
BANNERTOOL_PATH := "/mnt/c/devkitPro/tools/bin/bannertool.exe"

.PHONY: all clean setup build

setup:
	@echo "creating dirs..."
	@mkdir -p $(CMAKE_BUILD_DIR)
	@mkdir -p $(OUTPUT_DIR)
	@mkdir -p $(ROMFS_DIR)
	@echo "loading cmake..."
	@cmake -S . -B $(CMAKE_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(CMAKE_TOOLCHAIN) \

build:
	@echo "Building 3dsx and elf with cmake..."
	@cmake --build $(CMAKE_BUILD_DIR)
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(OUTPUT_DIR)
	@mv ./$(CMAKE_BUILD_DIR)/*.3dsx ./$(OUTPUT_DIR)
#	@cp $(CMAKE_BUILD_DIR)/*.elf $(OUTPUT_DIR)/App.elf

clean:
	@echo "Removing $(BUILD_DIR) and $(OUTPUT_DIR)..."
	@rm -rf $(BUILD_DIR)
	@rm -rf $(OUTPUT_DIR)

run:
	@echo "Launching citra..."
	@$(CITRA_PATH) $(OUTPUT_DIR)/*.3dsx

wav2raw:
	@mkdir -p resources/romfs/audio
	@for f in resources/assets/audio/*.wav; do \
		o="resources/romfs/audio/$$(basename "$${f%.wav}").raw"; \
		if [ -e "$$o" ]; then \
			echo "SKIPPING $$f (already exists)"; \
		else \
			echo "CONVERT $$f -> $$o"; \
			ffmpeg -i "$$f" -f s16le "$$o"; \
		fi; \
	done

runcia:
	@echo "Launching citra from cia file..."
	@$(CITRA_PATH) $(BUILD_DIR)/*.cia

cia:
	@$(MAKEROM_PATH) -f cia -o $(BUILD_DIR)/*.cia -elf $(CMAKE_BUILD_DIR)/*.elf -rsf rsf.yml