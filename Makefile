#
# XYScope Makefile
#

# Detect operating system
UNAME_S := $(shell uname -s)

SRC = xyscope.mm
ifeq ($(UNAME_S),Darwin)
    RELEASE_DIR = release/macOS
else
    RELEASE_DIR = release/linux
endif
BINARY = $(RELEASE_DIR)/xyscope
CALIBRATE = $(RELEASE_DIR)/xyscope-calibrate
CALIBRATE_SRC = xyscope-calibrate.mm
APP_NAME = $(RELEASE_DIR)/XYScope.app
APP_CONTENTS = $(APP_NAME)/Contents
APP_MACOS = $(APP_CONTENTS)/MacOS
APP_RESOURCES = $(APP_CONTENTS)/Resources
RESOURCES_SRC = resources

ifeq ($(UNAME_S),Darwin)
    # macOS
    CXX = clang++
    CXX_FLAGS = -Wall -O3 -std=c++11 -fobjc-arc -I/opt/homebrew/include
    LD_LIBS = -lpthread -L/opt/homebrew/lib -lSDL2 -lSDL2_ttf -framework OpenGL -framework Accelerate -framework Foundation -framework CoreAudio -framework AudioToolbox
else
    # Linux
    CXX = g++
    PIPEWIRE_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3)
    PIPEWIRE_LIBS = $(shell pkg-config --libs libpipewire-0.3)
    CXX_FLAGS = -Wall -O3 -march=native -mtune=native -std=c++11 -x c++ $(PIPEWIRE_CFLAGS)
    LD_LIBS = -lpthread -lSDL2 -lSDL2_ttf -lGL $(PIPEWIRE_LIBS) -lfftw3
endif

# Default target: build binary + calibrate (+ app bundle on macOS)
ifeq ($(UNAME_S),Darwin)
all: $(BINARY) $(CALIBRATE) app
else
all: $(BINARY) $(CALIBRATE)
endif

# Build xyscope binary
$(BINARY): $(SRC) Makefile
	@mkdir -p $(RELEASE_DIR)
	@echo "Building xyscope binary..."
	$(CXX) $(CXX_FLAGS) $(SRC) $(LD_LIBS) -o $(BINARY)
	@echo "✓ xyscope binary built → $(BINARY)"

# Build calibration tool
$(CALIBRATE): $(CALIBRATE_SRC) xyscope-shared.h xyscope-ringbuffer.h xyscope-draw.h Makefile
	@mkdir -p $(RELEASE_DIR)
	@echo "Building xyscope-calibrate..."
ifeq ($(UNAME_S),Darwin)
	clang++ -Wall -O3 -std=c++11 -I/opt/homebrew/include $(CALIBRATE_SRC) -L/opt/homebrew/lib -lSDL2 -lm -framework OpenGL -o $(CALIBRATE)
else
	g++ -Wall -O3 -std=c++11 -x c++ $(CALIBRATE_SRC) -lSDL2 -lGL -lm -o $(CALIBRATE)
endif
	@echo "✓ xyscope-calibrate built → $(CALIBRATE)"

# Assemble .app bundle (macOS only)
.PHONY: app
app: $(BINARY)
	@echo "Assembling $(APP_NAME) bundle..."
	@mkdir -p $(APP_MACOS) $(APP_RESOURCES)
	@mv $(BINARY) $(APP_MACOS)/xyscope-bin
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
	rm -f core *.o
	rm -rf $(RELEASE_DIR)
	@echo "✓ Clean complete"

# Full rebuild
rebuild: clean all

# Help
.PHONY: help
help:
	@echo "XYScope Build System"
	@echo "===================="
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build binaries (+ .app bundle on macOS)"
	@echo "  make app      - Assemble .app bundle (macOS only)"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make rebuild  - Clean and rebuild everything"
	@echo ""

.PHONY: all clean rebuild help
