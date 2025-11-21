#!/bin/bash
#
# XYScope - Smart launcher with automatic setup
#

cd "$(dirname "$0")"

echo "XYScope Audio Visualizer"
echo "========================"
echo

# Check for Homebrew
if ! command -v brew >/dev/null 2>&1; then
    echo "Error: Homebrew is required but not installed."
    echo "Please install Homebrew from https://brew.sh"
    echo
    read -p "Press Enter to exit..."
    exit 1
fi

# Check and install dependencies
echo "Checking dependencies..."

MISSING_DEPS=""
if ! brew list sdl2 >/dev/null 2>&1; then
    MISSING_DEPS="$MISSING_DEPS sdl2"
fi
if ! brew list sdl2_ttf >/dev/null 2>&1; then
    MISSING_DEPS="$MISSING_DEPS sdl2_ttf"
fi
if ! brew list blackhole-2ch >/dev/null 2>&1; then
    MISSING_DEPS="$MISSING_DEPS blackhole-2ch"
fi

FIRST_TIME_SETUP=false
if [ -n "$MISSING_DEPS" ]; then
    FIRST_TIME_SETUP=true
    echo "Missing dependencies:$MISSING_DEPS"
    echo "Installing via Homebrew..."
    brew install$MISSING_DEPS

    # After installing BlackHole, restart CoreAudio
    if [[ "$MISSING_DEPS" == *"blackhole-2ch"* ]]; then
        sleep 2
        echo "Restarting CoreAudio..."
        sudo killall coreaudiod 2>/dev/null || true
        sleep 2
    fi

    echo "Dependencies installed!"
    echo
fi

# Check if multi-output device exists
if ! system_profiler SPAudioDataType | grep -q "Multi-Output Device"; then
    FIRST_TIME_SETUP=true
    echo "Multi-Output Device not found!"
    echo

    # Open Audio MIDI Setup first
    open "/System/Applications/Utilities/Audio MIDI Setup.app"
    sleep 1

    # Show instructions dialog that stays on top
    (
    nohup osascript <<'EOF'
tell application "System Events"
    display dialog "Audio Setup Instructions

Follow these steps in Audio MIDI Setup:

1. Click the '+' button at bottom-left
2. Select 'Create Multi-Output Device'
3. Check BOTH boxes:
   ☑ MacBook Pro Speakers (must be first!)
   ☑ BlackHole 2ch
4. Select Multi-Output Device in sidebar
5. Set Format → Sample Rate to 96000 Hz
6. Right-click Multi-Output Device
   → 'Use This Device For Sound Output'
7. Close Audio MIDI Setup
8. Run XYScope.command again

Click OK when finished with setup." buttons {"OK"} default button 1 with title "XYScope Setup"
end tell
EOF
    ) &

    osascript -e 'tell application "Terminal" to close (every window whose name contains "XYScope.command")' >/dev/null 2>&1 &
    exit 0
fi

# Launch xyscope detached from terminal
nohup ../MacOS/xyscope-bin >/dev/null 2>&1 &

# Wait a few seconds for audio connection
sleep 3

osascript -e 'tell application "Terminal" to close (every window whose name contains "XYScope.command")' >/dev/null 2>&1 &
