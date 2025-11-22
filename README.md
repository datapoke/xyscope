# XYScope - Audio Visualizer

Real-time XY oscilloscope visualization of stereo audio using OpenGL and BlackHole virtual audio driver.

## Quick Start

### For Users (Running the App)

1. **Double-click `XYScope.app`**
   - Opens Terminal and installs dependencies automatically
   - Installs BlackHole audio driver via Homebrew
   - Shows instructions for Audio MIDI Setup

2. **Configure Multi-Output Device** (one time only):
   - Audio MIDI Setup will open automatically
   - Click the **+** button at bottom-left
   - Select **"Create Multi-Output Device"**
   - Check both:
     - ☑ **MacBook Pro Speakers** (or your speakers) - must be first
     - ☑ **BlackHole 2ch**
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
- Homebrew with SDL2, SDL2_ttf, and BlackHole 2ch

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
├── resources/              App bundle resources
│   ├── Info.plist
│   └── XYScope.command
└── XYScope.app/            Built app (ready to distribute)
```

**After building**, you can:
- Move `XYScope.app` to `/Applications`
- All sources remain in project directory for rebuilding
- The .app is self-contained and automatically installs BlackHole

## How It Works

```
Audio Source → Multi-Output Device → BlackHole 2ch (visualization)
                                  → Speakers (audio output)
```

The multi-output device sends audio to both:
- **BlackHole 2ch** - Virtual audio device captured for visualization
- **Your speakers** - So you can hear the audio

The visualizer reads from the BlackHole virtual device at **96kHz sample rate** and renders Lissajous curves in real-time using OpenGL.

**BlackHole** is an open-source virtual audio driver by [Existential Audio](https://existential.audio/blackhole/). XYScope automatically installs it via Homebrew.

## Features

- **5 Display Modes**: Standard, Radius, Length, Frequency, Time
- **Frequency Mode**: STFT-based spectral analysis coloring
- **Catmull-Rom Spline**: Smooth curve interpolation
- **Auto-scaling**: Automatic amplitude adjustment
- **Preferences**: Saved settings for window, zoom, colors

## Keyboard Controls

See `CLAUDE.md` for full keyboard reference.

## Advanced

**Manual Dependency Installation:**
```bash
brew install sdl2 sdl2_ttf blackhole-2ch
```

**BlackHole Driver:**
- Installed via Homebrew: `brew install blackhole-2ch`
- Location: `/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver`
- Project: https://existential.audio/blackhole/

## Troubleshooting

**No audio visualization (black screen)?**
- Check Multi-Output Device is selected as system output
- Verify sample rate is set to **96000 Hz** in Audio MIDI Setup
- Ensure both Speakers and BlackHole 2ch are checked in Multi-Output Device

**BlackHole not appearing?**
- Run: `brew install blackhole-2ch`
- Restart CoreAudio: `sudo killall coreaudiod`
- Check driver: `ls /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver`

## Uninstalling

```bash
brew uninstall blackhole-2ch sdl2 sdl2_ttf
```

Remove the Multi-Output Device in Audio MIDI Setup.

## License

See LICENSE file for details.
