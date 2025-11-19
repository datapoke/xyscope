# XYScope - Audio Visualizer with Virtual Audio Device

Real-time XY oscilloscope visualization of stereo audio using OpenGL and a virtual audio driver.

## Quick Start

### For Users (Running the App)

1. **Double-click `XYScope.app`**
   - Opens Terminal and guides you through setup
   - Installs the audio driver (requires password)
   - Shows instructions for Audio MIDI Setup

2. **Configure Multi-Output Device** (one time only):
   - Audio MIDI Setup will open automatically
   - Click the **+** button at bottom-left
   - Select **"Create Multi-Output Device"**
   - Check both:
     - ☑ **MacBook Pro Speakers** (or your speakers) - must be first
     - ☑ **XYScope 2ch**
   - **Select Multi-Output Device in sidebar**
   - **Set Sample Rate to 96000 Hz (96 kHz)** ⚠️ Important!
   - Right-click Multi-Output Device → **"Use This Device For Sound Output"**
   - Close Audio MIDI Setup

3. **Run XYScope.command again**
   - The visualizer will launch
   - Play some audio (music, YouTube, etc.) to see the visualization

### For Developers (Building from Source)

**Build Requirements:**
- macOS 10.15+ with Xcode Command Line Tools
- Homebrew with SDL2 and SDL2_ttf

**Build Commands:**
```bash
make              # Build everything and assemble .app bundle
make clean        # Clean build artifacts
make rebuild      # Clean and rebuild everything
```

**Project Structure:**
```
xyscope/
├── xyscope.mm              Main visualizer source
├── Makefile                Master build file
├── driver/                 CoreAudio driver sources
│   ├── BlackHole.c
│   ├── BlackHole.plist
│   └── Makefile
├── resources/              App bundle resources
│   ├── Info.plist
│   ├── XYScope.command
│   └── install.sh
└── XYScope.app/            Built app (ready to distribute)
```

**After building**, you can:
- Move `XYScope.app` to `/Applications`
- All sources remain in project directory for rebuilding
- The .app is self-contained and includes all necessary files

## How It Works

```
Audio Source → Multi-Output Device → XYScope 2ch (visualization)
                                  → Speakers (audio output)
```

The multi-output device sends audio to both:
- **XYScope 2ch** - Virtual audio device captured for visualization
- **Your speakers** - So you can hear the audio

The visualizer reads from the XYScope virtual device at **96kHz sample rate** and renders Lissajous curves in real-time using OpenGL.

## Features

- **5 Display Modes**: Standard, Radius, Length, Frequency, Time
- **Frequency Mode**: STFT-based spectral analysis coloring
- **Catmull-Rom Spline**: Smooth curve interpolation
- **Auto-scaling**: Automatic amplitude adjustment
- **Preferences**: Saved settings for window, zoom, colors

## Keyboard Controls

See `CLAUDE.md` for full keyboard reference.

## Advanced

**Manual Driver Installation:**
```bash
make install-driver    # Install driver (requires sudo)
make uninstall-driver  # Uninstall driver (requires sudo)
```

**Driver Location:**
- Installed: `/Library/Audio/Plug-Ins/HAL/XYScope.driver`
- Source: `driver/BlackHole.c` (based on BlackHole virtual audio driver)

## Troubleshooting

**No audio visualization (black screen)?**
- Check Multi-Output Device is selected as system output
- Verify sample rate is set to **96000 Hz** in Audio MIDI Setup
- Ensure both Speakers and XYScope 2ch are checked in Multi-Output Device

**Driver not appearing?**
- Restart Mac after driver installation
- Check driver is installed: `ls /Library/Audio/Plug-Ins/HAL/XYScope.driver`

## Uninstalling

```bash
make uninstall-driver
```

Or manually:
```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/XYScope.driver
sudo killall -9 coreaudiod
```

Remove the Multi-Output Device in Audio MIDI Setup.

## License

See LICENSE file for details.
