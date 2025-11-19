# XYScope - Audio Visualizer with Virtual Audio Device

## Quick Start

### First Time Setup

1. **Install the driver** (one time only):
   ```bash
   ./install.sh
   ```
   This installs XYScope.driver to `/Library/Audio/Plug-Ins/HAL/`

2. **Set up audio routing** (one time only):
   - Open **Audio MIDI Setup** (in `/Applications/Utilities/`)
   - Click the **+** button at bottom-left
   - Select **"Create Multi-Output Device"**
   - Check both:
     - ☑ **MacBook Pro Speakers** (or your speakers) - must be first
     - ☑ **XYScope 2ch**
   - Right-click the Multi-Output Device → **"Use This Device For Sound Output"**
   - Close Audio MIDI Setup

### Daily Use

Just double-click **`XYScope.command`**!

It will open a Terminal window and launch the visualizer. Play some audio (music, YouTube, etc.) to see the visualization.

## How It Works

```
Browser/App → Multi-Output Device → XYScope 2ch (visualization)
                                  → Your Speakers (audio output)
```

The multi-output device sends audio to both:
- **XYScope 2ch** - Captured for visualization
- **Your speakers** - So you can hear it

## Files

- `install.sh` - One-time driver installation
- `XYScope.command` - Double-click to run (opens in Terminal)
- `xyscope` - The visualizer binary
- `XYScope.driver/` - Source for the audio driver

## Uninstalling

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/XYScope.driver
sudo killall -9 coreaudiod
```

Remove the Multi-Output Device in Audio MIDI Setup.
