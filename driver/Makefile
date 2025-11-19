# XYScope CoreAudio Driver Makefile

DRIVER_NAME = XYScope
BUNDLE_NAME = $(DRIVER_NAME).driver
SOURCE = BlackHole.c
EXECUTABLE = $(DRIVER_NAME)

# Build settings
CC = clang
CFLAGS = -O3 -std=c11 -framework CoreAudio -framework CoreFoundation -framework Accelerate
BUNDLE_FLAGS = -bundle

# Bundle structure - output to parent Resources directory
BUNDLE_DIR = ../$(BUNDLE_NAME)
CONTENTS_DIR = $(BUNDLE_DIR)/Contents
MACOS_DIR = $(CONTENTS_DIR)/MacOS

all: $(BUNDLE_DIR)

$(BUNDLE_DIR): $(SOURCE) BlackHole.plist
	@echo "Building $(DRIVER_NAME) driver..."

	# Create bundle structure
	mkdir -p $(MACOS_DIR)

	# Compile driver
	$(CC) $(CFLAGS) $(BUNDLE_FLAGS) -o $(MACOS_DIR)/$(EXECUTABLE) $(SOURCE) -DkDriver_Name=\"$(DRIVER_NAME)\" -DkPlugIn_BundleID=\"audio.xyscope.$(DRIVER_NAME)\"

	# Process Info.plist with variable substitution
	sed -e 's/$${EXECUTABLE_NAME}/$(EXECUTABLE)/g' \
	    -e 's/$$(PRODUCT_BUNDLE_IDENTIFIER)/audio.xyscope.$(DRIVER_NAME)/g' \
	    -e 's/$${PRODUCT_NAME}/$(DRIVER_NAME)/g' \
	    -e 's/$$(MARKETING_VERSION)/1.0.0/g' \
	    BlackHole.plist > $(CONTENTS_DIR)/Info.plist

	@echo "Build complete: $(BUNDLE_DIR)"

clean:
	rm -rf $(BUNDLE_DIR)
	rm -f *.o

install: $(BUNDLE_DIR)
	@echo "Installing $(BUNDLE_NAME)..."
	sudo cp -R $(BUNDLE_DIR) /Library/Audio/Plug-Ins/HAL/
	@echo "Restarting CoreAudio..."
	sudo killall -9 coreaudiod
	@echo "Installation complete!"

uninstall:
	@echo "Uninstalling $(BUNDLE_NAME)..."
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/$(BUNDLE_NAME)
	@echo "Restarting CoreAudio..."
	sudo killall -9 coreaudiod
	@echo "Uninstall complete!"

.PHONY: all clean install uninstall
