# XYScope - Full Detail Audio Visualizer

Real-time XY oscilloscope visualization of stereo audio using OpenGL.
Supports macOS (CoreAudio/BlackHole), Linux (Pipewire), and Windows (WASAPI loopback).

## Quick Start

### macOS

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

3. **Run XYScope.app again**
   - The visualizer will launch
   - Play some audio (music, YouTube, etc.) to see the visualization

### Linux

Install dependencies and build:
```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev libpipewire-0.3-dev libfftw3-dev
make
./release/xyscope
```

No virtual audio device needed — Pipewire captures system audio directly.

### Windows

No virtual audio device needed — WASAPI loopback captures system audio directly.

**Cross-compile from Linux using Docker:**
```bash
./build_windows.sh
```

This produces `release/xyscope.exe` with all required DLLs in `release/`.

**Native build with MSVC + vcpkg:**
```bash
vcpkg install sdl2 sdl2-ttf fftw3
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Building from Source

### macOS
```bash
brew install sdl2 sdl2_ttf blackhole-2ch
make
```

### Linux
```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev libpipewire-0.3-dev libfftw3-dev
make
```

### CMake (all platforms)
```bash
cmake -B build
cmake --build build
```

## How It Works

| Platform | Audio Capture | Virtual Device Required? |
|----------|--------------|------------------------|
| macOS    | CoreAudio → BlackHole 2ch | Yes (BlackHole) |
| Linux    | Pipewire loopback | No |
| Windows  | WASAPI loopback | No |

The visualizer reads stereo audio samples and renders Lissajous curves in real-time using OpenGL. Left channel maps to X, right channel maps to Y.

**macOS** requires a Multi-Output Device to route audio to both speakers and BlackHole:
```
Audio Source → Multi-Output Device → BlackHole 2ch (visualization)
                                   → Speakers (audio output)
```

**Linux and Windows** capture system audio output directly with no extra configuration.

## Features

- **5 Display Modes**: Standard, Radius, Length, Frequency, Time
- **Frequency Mode**: STFT-based spectral analysis coloring
- **Catmull-Rom Spline**: Smooth curve interpolation
- **Auto-scaling**: Automatic amplitude adjustment
- **Preferences**: Saved settings for window, zoom, colors

## Keyboard Controls

| Key | Action |
|-----|--------|
| Escape | Quit |
| F1–F5 | Resize window / fullscreen |
| Home/Page Up | Zoom in |
| End/Page Down | Zoom out |
| 0–9 | Set zoom factor |
| Spacebar | Pause/Resume |
| < > | Rewind/Fast-forward (when paused) |
| [ ] | Adjust color range |
| - + | Adjust color rate |
| a | Toggle auto-scale |
| b B | Adjust splines |
| c C | Cycle color mode |
| d D | Cycle display mode |
| f | Toggle fullscreen |
| h | Show/hide help |
| r | Recenter |
| s S | Show/hide statistics |
| w W | Adjust line width |

## Project Structure

```
xyscope/
├── xyscope.mm              Main visualizer source (all platforms)
├── Makefile                Build file (macOS/Linux)
├── CMakeLists.txt          CMake build (all platforms)
├── Dockerfile.windows      Docker cross-compilation for Windows
├── build_windows.sh        One-step Windows cross-compilation
├── release/                Build output (gitignored)
│   ├── xyscope             Linux/macOS binary
│   ├── XYScope.app/        macOS app bundle
│   └── xyscope.exe + DLLs  Windows build
└── resources/              macOS app bundle resources
    ├── Info.plist
    └── XYScope.command
```

## Troubleshooting

### macOS

**No audio visualization (black screen)?**
- Check Multi-Output Device is selected as system output
- Verify sample rate is set to **96000 Hz** in Audio MIDI Setup
- Ensure both Speakers and BlackHole 2ch are checked in Multi-Output Device

**BlackHole not appearing?**
- Run: `brew install blackhole-2ch`
- Restart CoreAudio: `sudo killall coreaudiod`
- Check driver: `ls /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver`

### Windows

**No audio visualization?**
- Make sure audio is actually playing through the default output device
- Check that no other application has exclusive access to the audio device

### Linux

**No audio visualization?**
- Ensure Pipewire is running: `systemctl --user status pipewire`
- Check that audio is playing through Pipewire (not raw ALSA)

## Uninstalling

**macOS:**
```bash
brew uninstall blackhole-2ch sdl2 sdl2_ttf
```
Remove the Multi-Output Device in Audio MIDI Setup.

**Linux:**
```bash
sudo apt remove libsdl2-dev libsdl2-ttf-dev libpipewire-0.3-dev libfftw3-dev
```

## License

See LICENSE file for details.
