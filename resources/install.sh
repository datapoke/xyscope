#!/bin/bash
#
# XYScope Installation Script
# Installs the XYScope.driver to system audio plugins
#

set -e

echo "╔════════════════════════════════════════╗"
echo "║     XYScope Driver Installation        ║"
echo "╚════════════════════════════════════════╝"
echo ""

# Check if driver exists (we're running from Resources directory)
if [ ! -d "XYScope.driver" ]; then
    echo "Error: XYScope.driver not found!"
    echo "Please build it first by running 'make' from the project root."
    exit 1
fi

# Install driver
echo "Installing XYScope.driver to /Library/Audio/Plug-Ins/HAL/..."
sudo cp -R XYScope.driver /Library/Audio/Plug-Ins/HAL/

echo "Restarting CoreAudio..."
sudo killall -9 coreaudiod

echo ""
echo "✓ Installation complete!"
echo ""
echo "Next steps:"
echo "  1. Wait a few seconds for CoreAudio to restart"
echo "  2. Double-click XYScope.app to run the visualizer"
echo ""
