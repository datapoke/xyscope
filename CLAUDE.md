# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

XYScope is an OpenGL-based audio visualizer that creates real-time XY plots (Lissajous curves) from stereo audio input. It's a single-file C++ application that combines audio processing, FFT analysis, and OpenGL rendering.

**Platform Support:**
- **macOS**: Uses CoreAudio for input, Accelerate framework for FFT, native OpenGL/GLUT
- **Linux**: Uses JACK Audio for input, FFTW3 for FFT, system OpenGL/GLUT

## Build Commands

**Build the application:**
```bash
make
```
The Makefile automatically detects the OS (Darwin/Linux) and uses appropriate compiler flags and frameworks.

**macOS build requirements:**
- Stock macOS (no external dependencies)
- Uses frameworks: GLUT, OpenGL, CoreAudio, AudioToolbox, Accelerate

**Linux build requirements:**
- JACK Audio Connection Kit (libjack-dev)
- FFTW3 (libfftw3-dev)
- OpenGL/GLUT (freeglut3-dev)

**Clean build artifacts:**
```bash
make clean
```

**Docker build and run (Linux only):**
```bash
./build.sh
```

**Remove Docker image:**
```bash
./clean.sh
```

## Architecture

### Main Components

**Audio Input Thread (`audioInput` class)**
- Runs in a separate pthread created in constructor
- **macOS**: Uses CoreAudio HAL Output Audio Unit configured for input
  - Automatically connects to default system input device
  - Callback-based architecture via `audioInputCallback()`
  - Custom ringbuffer implementation for thread-safe audio data transfer
- **Linux**: Connects to JACK audio server as client "xyscope"
  - Registers two input ports: "in1" (left) and "in2" (right)
  - Auto-connects to ports containing "output_FL" and "output_FR" in their names
  - Uses jack_ringbuffer for lockless communication
  - Automatically reconnects when new JACK ports become available (0.5s debounce)

**Scene Rendering (`scene` class)**
- Main visualization engine
- Owns the audioInput instance
- Manages OpenGL display state and preferences
- Implements 5 display modes: Standard, Radius, Length, Frequency, Time
- Frequency mode performs Short-Time Fourier Transform (STFT) for spectrum-based coloring
  - **macOS**: Uses Accelerate framework's vDSP FFT functions
  - **Linux**: Uses FFTW3 library
- Supports Catmull-Rom spline interpolation for smooth curves
- Auto-scaling with max sample tracking and smooth transitions

**Ring Buffer Architecture**
- Size: `SAMPLE_RATE * BUFFER_SECONDS + FRAMES_PER_BUF` frames
- Default: 96000 samples/sec × 1.0 sec = ~96000 frames (~768KB for stereo)
- Rounds up to next power of 2 for efficient modulo operations
- Audio callback writes interleaved stereo frames
- Display reads `FRAMES_PER_BUF` frames per render (default: 1600 frames)
- **macOS**: Custom implementation (lines 204-308)
- **Linux**: JACK's jack_ringbuffer

**Threading Model**
- Main thread: GLUT event loop + OpenGL rendering
- Audio thread:
  - **macOS**: CoreAudio callback (system-managed real-time thread)
  - **Linux**: JACK process callback (real-time priority)
- Reader thread: pthread waiting on condition variable for new audio data
- Synchronization via pthread_mutex and pthread_cond for ringbuffer access

### Key Configuration Constants (lines 44-96)

- `SAMPLE_RATE`: 96000 Hz (adjust to match your audio setup)
- `FRAME_RATE`: 60 FPS target
- `BUFFER_SECONDS`: 1.0 second ringbuffer
- `DRAW_EACH_FRAME`: 2 (renders each frame twice for brightness)
- `FRAMES_PER_BUF`: Samples drawn per frame = (SAMPLE_RATE / FRAME_RATE) × DRAW_EACH_FRAME
- `DEFAULT_SPLINE_STEPS`: 32 (Catmull-Rom interpolation points)

### Preferences System

Preferences are saved to `.xyscope.pref` on exit and loaded on startup (lines 1843-1846). The binary file stores the entire `preferences_t` struct including:
- Window dimensions and position
- Zoom/scale settings
- Display and color modes
- Line width, spline steps, auto-scale state

### Display Modes (lines 660-721)

1. **Standard**: Single color based on hue setting
2. **Radius**: Color varies by distance from origin
3. **Length**: Color varies by delta between consecutive samples
4. **Frequency**: STFT-based spectral analysis colors each segment by average magnitude
5. **Time**: Color gradient based on position in the frame buffer

**STFT Implementation** (Frequency mode):
- Window size: DRAW_FRAMES / 100
- Overlap: 50% (window_size - overlap_size)
- Uses FFTW3 library for FFT computation
- Colors mapped from average magnitude per window

### Color Modes (lines 486-492)

1. **Standard**: Static hue rotation at color_rate
2. **Delta**: Hue rotation speed based on signal delta (motion-reactive)

## Development Notes

**Dependencies:**
- OpenGL/GLUT (freeglut3-dev)
- JACK Audio (libjack-dev)
- FFTW3 for FFT (libfftw3-dev)

**Compiler flags:**
- `-O3 -march=native -mtune=native` for performance
- Links: `-lpthread -lglut -lGL -ljack -lfftw3`

**JACK Port Connection Logic** (lines 343-383):
- Scans all available JACK ports
- Connects ports with "output_FL" to in1 (left channel)
- Connects ports with "output_FR" to in2 (right channel)
- Prevents duplicate connections via `jack_port_connected_to()`

**Mouse/Keyboard Controls:**
- All keyboard handling in `keyboard()` function (lines 1688-1790)
- Special keys (F1-F5, arrows, Page Up/Down) in `special()` (lines 1640-1686)
- Mouse drag with left button to pan, right button to zoom (lines 1797-1831)

**Text Rendering:**
- Uses GLUT bitmap fonts (GLUT_BITMAP_HELVETICA_18)
- Timed text displays disappear after 10 seconds
- Stats overlay shows FPS, vertices per second, latency
- Counter shows playback position when paused

**Performance Considerations:**
- Single-file design keeps everything cache-friendly
- Ringbuffer uses power-of-2 sizing for efficient modulo
- Spline calculation only occurs when spline_steps > 1
- FFT plan creation/destruction happens per frame in Frequency mode (potential optimization target)
- Frame rate limiting via usleep() when RESPONSIBLE_FOR_FRAME_RATE is true

**Common Modification Points:**
- Change sample rate: Update `SAMPLE_RATE` constant
- Adjust visualization smoothness: Modify `DRAW_EACH_FRAME` or `DEFAULT_SPLINE_STEPS`
- Buffer size: Change `BUFFER_SECONDS` (affects memory and rewind capability)
- Add display modes: Extend the display_mode_handles enum and add case in drawPlot()
- **Linux only**: JACK port matching - Modify string matching in connectPorts()

## Platform-Specific Implementation Details

**Conditional Compilation:**
The codebase uses `#ifdef __APPLE__` to provide platform-specific implementations while maintaining a single source file.

**macOS-Specific Code:**
- **CoreAudio Setup** (lines 536-616): Configures HAL Output Audio Unit for input
  - Sets up stereo input at sample rate defined by `SAMPLE_RATE`
  - Enables input, disables output on the audio unit
  - Registers `audioInputCallback()` for real-time audio processing
- **Ring Buffer** (lines 204-308): Custom implementation using power-of-2 sizing
- **FFT** (lines 1035-1070): Uses vDSP functions from Accelerate framework
  - `vDSP_create_fftsetup()`: Create FFT setup
  - `vDSP_ctoz()`: Convert real to split-complex format
  - `vDSP_fft_zrip()`: Perform real-to-complex FFT
  - Results are mirrored for second half of spectrum

**Linux-Specific Code:**
- **JACK Setup**: Traditional JACK client registration and port connection
- **FFT**: FFTW3 library with plan-based execution

**Platform Detection in Makefile:**
- Uses `uname -s` to detect operating system
- Automatically selects appropriate compiler (clang++ for macOS, g++ for Linux)
- Links correct frameworks/libraries based on platform
