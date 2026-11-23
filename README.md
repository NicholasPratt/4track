# 8track

An 8-track recorder with clip-based, non-destructive editing, built with C++ and JUCE.

## Features

- **8 tracks** with arm, mute, solo, input monitoring (off / auto / on), and a mono, stereo or master-mix (bounce) input
- **Clips, non-destructive.** Every recording pass is a clip pointing at its own WAV. Move, trim, split, fade, change gain, delete, with undo/redo. Recording over existing audio trims what's underneath and never touches the file.
- **Punch in/out.** Drag a range on the lower strip of the ruler, turn Punch on, press Record. You get one bar of pre-roll, and capture starts and stops on the exact sample.
- **Latency compensation.** Takes are shifted by the device's input + output latency, so overdubs line up with what you heard.
- **Tempo grid** with bars/beats ruler, snapping, metronome and count-in
- **Mixer** with low cut, high cut, mid peak EQ, a reverb send, pan, faders and dB-scaled meters; a master strip with reverb controls
- **Projects** are folders (`project.8track` + `Audio/`). Opening one at a different sample rate converts it. Projects saved by 4track (the earlier name) still open.
- **Export mix** renders offline through the same mixer code as playback, including the reverb tail.

## Keyboard

| Key | Action | Key | Action |
|---|---|---|---|
| Space | Play / stop (stop returns to where you started) | R | Record / stop recording |
| Return | Go to start | ← / → | Move playhead by grid step (Shift: by bar) |
| S | Split selected clips at the playhead | Delete | Delete selected clips |
| P | Punch on/off | I / O | Set punch in / out at the playhead |
| C | Metronome | X | Show/hide mixer |
| = / - / Z | Zoom in / out / to fit | Cmd+Z / Shift+Cmd+Z | Undo / redo |
| Cmd+S / Shift+Cmd+S | Save / Save As | Cmd+O / Cmd+N | Open / New |
| Cmd+I / Cmd+E | Import audio / Export mix | Cmd+, | Audio settings |

Mouse: drag a clip to move it (to other tracks too), drag its edges to trim, drag its top corners to fade, and right-click for more. Hold Cmd while dragging to bypass snap. Cmd+scroll or pinch to zoom. Drop audio files onto a track to import them.

## Building

Requires CMake 3.22+ and a C++20 compiler (Xcode 14+). Tracktion Engine is a git submodule and
brings the JUCE version it needs (9.0.5):

```bash
git submodule update --init tracktion_engine
# Tracktion's JUCE submodule uses an SSH URL; this fetches it over HTTPS instead:
git -C tracktion_engine config submodule.modules/juce.url https://github.com/juce-framework/JUCE.git
git -C tracktion_engine submodule update --init modules/juce
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
open build/8Track_artefacts/Debug/8track.app
```

Tests (model, mixer, file I/O, and recording driven through the audio callback without a device):

```bash
cmake --build build --target 8TrackTests && ./build/8TrackTests_artefacts/Debug/8TrackTests
```

## Source layout

```
Source/
├── Model/      Clip, Arrangement (lanes of non-overlapping clips, edit operations), EditHistory
├── Audio/      MixEngine (shared by playback and export), AudioEngine (device, transport, recording,
│               monitoring, metronome), Parameters (atomics shared with the audio thread)
├── Project/    Project (undo, save/load, import/export), AudioFileIO (WAV I/O, resampling)
└── UI/         MainComponent, TransportBar, ArrangeView (Ruler, TrackHeader, TrackLanes), MixerPanel
Tests/          Unit tests (juce::UnitTest)
```

The audio thread never allocates, locks or touches the disk. Recordings stream to disk through
`AudioFormatWriter::ThreadedWriter`, and the arrangement reaches the audio thread as an immutable
snapshot that is swapped under the device's callback lock.
