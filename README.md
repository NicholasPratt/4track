# 4track - Vintage 4-Track Tape Recorder

A cross-platform desktop audio recorder inspired by classic 4-track tape recorders, built with C++ and JUCE.

## Features

- **4 Independent Tracks** - Record and playback on 4 separate tracks simultaneously
- **Vintage UI** - Minimal black and white interface with analog-style VU meters
- **Transport Controls** - Play, Stop (Pause), Record, RTZ (Return to Zero), FFD (Fast Forward), RWD (Rewind)
- **Per-Track Controls**:
  - ARM - Arm track for recording
  - M - Mute track
  - S - Solo track
  - Volume slider (vertical)
  - Pan control (rotary knob)
  - VU meter with needle animation
- **Real-time Audio Processing** - Lock-free audio engine for low-latency recording and playback

## Requirements

- CMake 3.15+
- C++17 compatible compiler
- JUCE Framework v7.0.12
- macOS (CoreAudio), Windows (ASIO), or Linux (ALSA/JACK)

## Building

1. Clone JUCE framework:
```bash
git clone https://github.com/juce-framework/JUCE.git
cd JUCE
git checkout 7.0.12
cd ..
```

2. Create build directory and build:
```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

3. Run the application:
- **macOS**: `open 4Track_artefacts/Debug/4track.app`
- **Windows**: `4Track_artefacts\Debug\4track.exe`
- **Linux**: `./4Track_artefacts/Debug/4track`

## Project Structure

```
4track/
├── CMakeLists.txt              # Build configuration
├── Source/
│   ├── Main.cpp                # Entry point
│   ├── Audio/                  # Audio engine and processing
│   │   ├── AudioEngine.cpp     # Main audio I/O
│   │   ├── Track.cpp           # Individual track implementation
│   │   ├── TrackManager.cpp    # Multi-track coordination
│   │   └── TransportController.cpp  # Transport state management
│   └── UI/                     # User interface components
│       ├── MainComponent.cpp   # Root UI component
│       ├── TrackStrip.cpp      # Per-track UI strip
│       ├── TransportBar.cpp    # Transport controls
│       └── VUMeter.cpp         # Analog VU meter
└── JUCE/                       # JUCE Framework (not included)
```

## Version History

- **v0.4.0** - Initial release with core recording/playback functionality

## Technical Details

- **Sample Rate**: 44.1kHz or 48kHz (configurable via audio device)
- **Audio Format**: 32-bit float, stereo
- **Buffer Size**: 512 samples (adjustable)
- **Recording Capacity**: ~5 minutes per track (expandable)
- **VU Meter Range**: -20 dB to +3 dB

## Future Enhancements

- VST3/AU plugin hosting (per-track)
- Effect sends and master bus
- Project save/load (XML)
- Audio file export (WAV)
- Waveform display
- Undo/redo support

## License

[Add your license here]

## Credits

Built with [JUCE Framework](https://juce.com/)
