#
# XYScope Master Makefile
# Builds xyscope binary and audio driver, assembles .app bundle
#

# Detect operating system
UNAME_S := $(shell uname -s)

SRC = xyscope.mm
BINARY = xyscope
APP_NAME = XYScope.app
APP_CONTENTS = $(APP_NAME)/Contents
APP_MACOS = $(APP_CONTENTS)/MacOS
APP_RESOURCES = $(APP_CONTENTS)/Resources
DRIVER_SRC = driver
DRIVER_BUNDLE = XYScope.driver
RESOURCES_SRC = resources

ifeq ($(UNAME_S),Darwin)
    # macOS
    CXX = clang++
    CXX_FLAGS = -Wall -O3 -std=c++11 -fobjc-arc -I/opt/homebrew/include
    LD_LIBS = -lpthread -L/opt/homebrew/lib -lSDL2 -lSDL2_ttf -framework OpenGL -framework Accelerate -framework Foundation -framework CoreAudio -framework AudioToolbox
else
    # Linux
    CXX = g++
    CXX_FLAGS = -Wall -O3 -march=native -mtune=native -std=c++11 -x c++
    LD_LIBS = -lpthread -lSDL2 -lSDL2_ttf -lGL -ljack -lfftw3
endif

# Default target: build everything
all: $(BINARY) app

# Build xyscope binary
$(BINARY): $(SRC) Makefile
	@echo "Building xyscope binary..."
	$(CXX) $(CXX_FLAGS) $(SRC) $(LD_FLAGS) $(LD_LIBS) -o $(BINARY)
	@echo "✓ xyscope binary built"

# BlackHole audio driver is now installed via Homebrew (blackhole-2ch)
# No longer building custom driver

# Assemble .app bundle
.PHONY: app
app: $(BINARY)
	@echo "Assembling $(APP_NAME) bundle..."
	@mkdir -p $(APP_MACOS) $(APP_RESOURCES)
	@cp $(BINARY) $(APP_MACOS)/xyscope-bin
	@chmod +x $(APP_MACOS)/xyscope-bin
	@echo "Creating launcher script..."
	@printf '#!/bin/bash\n#\n# XYScope.app launcher - opens Terminal and runs setup/visualizer\n#\n\n# Get the Resources directory\nRESOURCES_DIR="$$(cd "$$(dirname "$$0")/../Resources" && pwd)"\n\n# Open Terminal and run the launch script\nosascript <<EOF\ntell application "Terminal"\n    activate\n    do script "cd '"'"'$$RESOURCES_DIR'"'"' && ./XYScope.command"\nend tell\nEOF\n' > $(APP_MACOS)/XYScope
	@chmod +x $(APP_MACOS)/XYScope
	@echo "Copying resources..."
	@cp $(RESOURCES_SRC)/Info.plist $(APP_CONTENTS)/
	@cp $(RESOURCES_SRC)/XYScope.command $(APP_RESOURCES)/
	@chmod +x $(APP_RESOURCES)/XYScope.command
	@echo "✓ $(APP_NAME) ready"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f core *.o $(BINARY)
	rm -f $(APP_MACOS)/xyscope-bin
	@echo "✓ Clean complete"

# Install driver to system (macOS only, requires sudo)
.PHONY: install-driver
install-driver: driver
ifeq ($(UNAME_S),Darwin)
	@echo "Installing driver to /Library/Audio/Plug-Ins/HAL/..."
	sudo cp -R $(DRIVER_BUNDLE) /Library/Audio/Plug-Ins/HAL/
	@echo "Restarting CoreAudio..."
	sudo killall -9 coreaudiod
	@echo "✓ Driver installed"
else
	@echo "⚠ Driver installation is macOS-only"
endif

# Uninstall driver from system (macOS only, requires sudo)
.PHONY: uninstall-driver
uninstall-driver:
ifeq ($(UNAME_S),Darwin)
	@echo "Uninstalling XYScope.driver..."
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/XYScope.driver
	@echo "Restarting CoreAudio..."
	sudo killall -9 coreaudiod
	@echo "✓ Driver uninstalled"
else
	@echo "⚠ Driver uninstall is macOS-only"
endif

# Full rebuild
rebuild: clean all

# Help
.PHONY: help
help:
	@echo "XYScope Build System"
	@echo "===================="
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build xyscope binary, driver, and assemble .app"
	@echo "  make xyscope      - Build just the xyscope binary"
	@echo "  make driver       - Build just the audio driver (macOS only)"
	@echo "  make app          - Assemble .app bundle"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make rebuild      - Clean and rebuild everything"
	@echo "  make install-driver   - Install driver to system (requires sudo, macOS only)"
	@echo "  make uninstall-driver - Uninstall driver from system (requires sudo, macOS only)"
	@echo ""

.PHONY: all clean rebuild help
