# XYScope - Full Detail Audio Visualizer

Real-time XY oscilloscope visualization of stereo audio using OpenGL.
Supports macOS (CoreAudio/BlackHole), Linux (Pipewire), and Windows (WASAPI loopback).

## Quick Start

### macOS

Install from the [latest release](https://github.com/datapoke/xyscope/releases) using `wget` to avoid macOS Gatekeeper:
```bash
wget https://github.com/datapoke/xyscope/releases/download/v1.8.1/XYScope-macOS-v1.8.1.zip
unzip XYScope-macOS-v1.8.1.zip
cp -r xyscope-1.8.1/XYScope.app /Applications/
```

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
   - **Set Sample Rate to 96000 Hz (96 kHz)**
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
./release/linux/xyscope
```

No virtual audio device needed — Pipewire captures system audio directly.

To target a specific Pipewire node:
```bash
./release/linux/xyscope -t alsa_output.pci-0000_0c_00.4.analog-stereo
```

Or set it permanently in `~/.config/xyscope/xyscope.conf`:
```ini
target=alsa_output.pci-0000_0c_00.4.analog-stereo
```

### Windows

No virtual audio device needed — WASAPI loopback captures system audio directly.

Download from [Releases](https://github.com/datapoke/xyscope/releases) or cross-compile from Linux using Docker:
```bash
./build-windows.sh
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

### Windows (cross-compile)
```bash
./build-windows.sh    # requires Docker
```

### Release builds (all platforms)
```bash
make release VERSION=1.8.1
```
Builds macOS natively, Linux and Windows via Docker, and packages release archives.

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

- **3 Display Modes**: Standard, Radius, Frequency (STFT spectral analysis)
- **2 Color Modes**: Standard (static hue rotation), Delta (motion-reactive)
- **Catmull-Rom Spline**: Smooth curve interpolation between samples
- **Particles Mode**: Point rendering with depth testing and alpha blending
- **Velocity Dim**: Phosphor-style fading for fast-moving segments
- **Auto-scaling**: Automatic amplitude adjustment
- **10 Presets**: Save and recall visualization settings
- **Calibration Tool**: `xyscope-calibrate` measures audio and display latency

## CLI Arguments

```
xyscope [-p preset] [-t target]
  -p, --preset N   Load preset N (0-9) on startup
  -t, --target ID  Pipewire target node name or serial (Linux only)
```

## Keyboard Controls

| Key | Action |
|-----|--------|
| Escape | Quit |
| F1-F5 | Resize window |
| Home / Page Up | Zoom in |
| End / Page Down | Zoom out |
| Shift+0-9 | Set zoom factor |
| ` | Load default settings |
| 0-9 | Load preset |
| Ctrl+0-9 | Save preset |
| Spacebar | Pause/Resume |
| < > | Rewind/Fast-forward (when paused) |
| [ ] | Adjust color range |
| - + | Adjust color rate |
| a | Toggle auto-scale |
| c C | Cycle color mode |
| d D | Cycle display mode |
| f | Toggle fullscreen |
| h | Show/hide help overlay |
| l L | Adjust spline steps |
| u/i U/I | Adjust brightness |
| v/b V/B | Adjust bloom intensity |
| j/k J/K | Adjust display delay |
| n/m N/M | Adjust velocity dim |
| p | Toggle particles mode |
| r | Recenter |
| s S | Show/hide statistics |
| w W | Adjust line width |

## Configuration

Settings are saved as an INI-style text file:

| Platform | Path |
|----------|------|
| Windows | `%APPDATA%\XYScope\xyscope.conf` |
| Linux | `~/.config/xyscope/xyscope.conf` |
| macOS | `~/.config/xyscope/xyscope.conf` |

## Project Structure

```
xyscope/
├── xyscope.mm              Main source (all platforms, single-file)
├── xyscope-shared.h        Types, constants, config file I/O
├── xyscope-draw.h          GL vertex drawing loop
├── xyscope-ringbuffer.h    Lock-free SPSC ring buffer
├── xyscope-hdr.h           HDR brightness detection
├── xyscope-calibrate.mm    Audio/display latency calibration tool
├── Makefile                Build (macOS native, Linux native)
├── Dockerfile              Linux build container
├── Dockerfile-windows      Windows cross-compile container
├── build-linux.sh          Docker Linux build script
├── build-windows.sh        Docker Windows build script
├── docker-entrypoint.sh    Linux container entrypoint
├── resources/
│   ├── Info.plist          macOS app bundle metadata
│   └── XYScope.command     macOS first-run setup script
└── release/                Build output (gitignored)
    ├── macOS/              macOS binary + XYScope.app
    ├── linux/              Linux binary
    └── windows/            Windows exe + DLLs
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
- Try targeting a specific node: `xyscope -t $(wpctl status | grep -m1 'RUNNING')`
- Use `pw-cli list-objects` or `wpctl status` to find node names

## License

See LICENSE file for details.
