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

# Check if driver exists
if [ ! -d "XYScope.app/Contents/Resources/XYScope.driver" ]; then
    echo "Driver not found. Building..."
    make driver
    echo ""
fi

# Install using make
echo "Installing XYScope.driver to /Library/Audio/Plug-Ins/HAL/..."
sudo cp -R XYScope.app/Contents/Resources/XYScope.driver /Library/Audio/Plug-Ins/HAL/

echo "Restarting CoreAudio..."
sudo killall -9 coreaudiod

echo ""
echo "✓ Installation complete!"
echo ""
echo "Next steps:"
echo "  1. Wait a few seconds for CoreAudio to restart"
echo "  2. Double-click XYScope.app to run the visualizer"
echo ""
