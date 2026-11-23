# 8track

An 8-track recorder with clip-based, non-destructive editing, built with C++ and JUCE.

## Features

- **8 tracks** with arm, mute, solo, input monitoring (off / auto / on), and an input: a mono channel, or a stereo pair set up under **Audio Settings > Stereo input pairs** (recording the master mix isn't back yet since the move to Tracktion)
- **Clips, non-destructive.** Every recording pass is a clip pointing at its own WAV. Move, trim, split, fade, change gain, delete, cut/copy/paste and duplicate, with undo/redo. Recording over existing audio trims what's underneath and never touches the file.
- **Punch in/out.** Drag a range on the lower strip of the ruler, turn Punch on, press Record. You get one bar of pre-roll, and capture starts and stops on the exact sample.
- **Latency compensation.** Takes are shifted by the device's input + output latency, so overdubs line up with what you heard.
- **Tempo grid** with bars/beats ruler, snapping, metronome and count-in
- **Mixer** with low cut, high cut, mid peak EQ, a reverb send, pan, faders and dB-scaled meters; a master strip with reverb controls
- **MIDI tracks.** Right-click a track's name to make it a MIDI track (while it has no clips). Pick its instrument (any VST3/AU instrument) where the input selector was. Arm it and record from any MIDI keyboard; new takes replace what they're recorded over, like audio. Double-click an empty spot on a MIDI track to add a one-bar clip, and double-click a MIDI clip to edit its notes in the piano roll (click to add, drag to move, drag the end to resize, double-click to delete; notes snap to sixteenths, hold Cmd for free placement). MIDI clips move, trim, split and delete like audio clips.
- **Automation.** Click a track's **A** button and choose a control: volume, pan, reverb send, the EQ, or any parameter of the track's instrument and plugins. Its curve is drawn over the track's clips; click to add a point, drag to move it (snaps to the grid; hold Cmd for free), double-click to delete, right-click to clear or show the clips again. Faders and knobs follow the automation during playback, and the export includes it.
- **Plugins (VST3 and AU).** Up to four effect plugins per track, after the EQ and before the fader, and four on the master bus (on the whole mix, before the master fader). Click **+** in a mixer strip to add one; click a plugin to open its window; right-click it to bypass or remove it. **File > Plugins...** scans for installed plugins (in a separate process, so a plugin that crashes during the scan can't take 8track down).
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
| Cmd+X / Cmd+C | Cut / copy selected clips | Cmd+V | Paste at the playhead, on the selected track |
| Cmd+D | Duplicate selected clips straight after themselves | | |

Mouse: drag a clip to move it (to other tracks too), drag its edges to trim, drag its top corners to fade, and right-click for more. Hold Cmd while dragging to bypass snap. Cmd+scroll or pinch to zoom. Drop audio files onto a track to import them.

## Building

Requires CMake 3.22+ and clang 16 or newer: Tracktion Engine uses C++20 features that Apple clang 15
(Xcode 15) can't compile. Xcode 16, or Homebrew's LLVM (`brew install llvm`), works. Tracktion Engine
is a git submodule and brings the JUCE version it needs (9.0.5):

```bash
git submodule update --init tracktion_engine
# Tracktion's JUCE submodule uses an SSH URL; this fetches it over HTTPS instead:
git -C tracktion_engine config submodule.modules/juce.url https://github.com/juce-framework/JUCE.git
git -C tracktion_engine submodule update --init modules/juce
# With Homebrew LLVM on an Intel Mac (use /opt/homebrew/opt/llvm on Apple silicon):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++
cmake --build build
open build/8Track_artefacts/Debug/8track.app
```

Tests (the arrangement model, and the project and mixer against a real Tracktion Edit):

```bash
cmake --build build --target 8TrackTests && ./build/8TrackTests_artefacts/Debug/8TrackTests
```

## Source layout

```
Source/
├── Model/      Clip, Arrangement: the clip view the UI edits (lanes of non-overlapping clips)
├── Audio/      AudioEngine: Tracktion Engine, the transport, recording and the mixer plugins,
│               kept in step with the UI's Parameters
├── Project/    Project: the Tracktion Edit, undo, save/load, import, export, older-format import
└── UI/         MainComponent, TransportBar, ArrangeView (Ruler, TrackHeader, TrackLanes), MixerPanel
Tests/          Unit tests (juce::UnitTest)
```

Audio, recording and mixing run in [Tracktion Engine](https://github.com/Tracktion/tracktion_engine)
(GPLv3). Each track's mixer is a chain of Tracktion plugins (high-pass, EQ, low-pass, volume & pan,
reverb send); a hidden ninth track returns the reverb. Clip edits are made on an `Arrangement` copy
and written back to the Edit as one undo step.
