#!/bin/bash
#
# XYScope - Smart launcher with automatic setup
#

cd "$(dirname "$0")"

echo "XYScope Audio Visualizer"
echo "========================"
echo

# Check if driver is installed
if [ ! -d "/Library/Audio/Plug-Ins/HAL/XYScope.driver" ]; then
    echo "XYScope driver not installed."
    echo
    read -p "Install driver now? (requires sudo password) [y/N]: " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        ./install.sh
        if [ $? -ne 0 ]; then
            echo "Driver installation failed!"
            exit 1
        fi
        echo "Driver installed successfully!"
        echo "NOTE: You may need to restart your Mac for the driver to appear."
        echo
    else
        echo "Cannot run without driver. Please run install.sh first."
        exit 1
    fi
fi

# Check if multi-output device exists
if ! system_profiler SPAudioDataType | grep -q "Multi-Output Device"; then
    echo "Multi-Output Device not found!"
    echo

    osascript <<'EOF'
tell application "System Events"
    display dialog "Audio Setup Required

XYScope needs a Multi-Output Device to route audio for visualization.

Steps to create it:

1. Audio MIDI Setup will open automatically
2. Click the '+' button at bottom-left
3. Select 'Create Multi-Output Device'
4. Check BOTH boxes:
   ☑ MacBook Pro Speakers (must be first!)
   ☑ XYScope 2ch
5. Right-click Multi-Output Device → 'Use This Device For Sound Output'
6. Close Audio MIDI Setup
7. Run XYScope.command again

Ready to open Audio MIDI Setup?" buttons {"Cancel", "Open Audio MIDI Setup"} default button 2

    if button returned of result is "Open Audio MIDI Setup" then
        do shell script "open '/System/Applications/Utilities/Audio MIDI Setup.app'"
    end if
end tell
EOF

    echo
    echo "After setting up the Multi-Output Device, run XYScope.command again."
    exit 0
fi

# Everything is set up - launch xyscope!
echo "Launching XYScope visualizer..."
echo "Play some audio to see the visualization!"
echo
exec ../MacOS/xyscope-bin
