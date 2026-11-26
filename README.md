# 8track

An 8-track recorder with clip-based, non-destructive editing, built with C++ and JUCE.

## Features

- **8 tracks** with arm, mute, solo, input monitoring (off / auto / on), and an input: a mono channel, or a stereo pair set up under **Audio Settings > Stereo input pairs** (recording the master mix isn't back yet since the move to Tracktion)
- **Clips, non-destructive.** Every recording pass is a clip pointing at its own WAV. Move, trim, split, fade, change gain, delete, cut/copy/paste and duplicate, with undo/redo. Recording over existing audio trims what's underneath and never touches the file.
- **Punch in/out.** Drag a range on the lower strip of the ruler, turn Punch on, press Record. You get one bar of pre-roll, and capture starts and stops on the exact sample.
- **Latency compensation.** Takes are shifted by the device's input + output latency, so overdubs line up with what you heard.
- **Tempo grid** with bars/beats ruler, snapping, metronome and count-in. The metronome icon toggles the click and the count-in icon toggles the recording lead-in. In the Tempo popup, **Record only** limits the regular metronome to takes. **Count in recording** and **Count in playback** independently add one bar of clicks before starting from stop.
- **Tuner.** Click the tuning fork and play a single note into an armed audio track. The tuner shows the clearest pitch from the armed inputs, with its note, frequency, and cents from standard tuning. It works with input monitoring off.
- **Tempo popup.** Click the tempo button in the toolbar for the tempo (drag, double-click to type, -/+, halve/double, Up/Down arrows), tap tempo (the Tap button, or the space bar while the popup is open), the time signature, and MIDI clock out: turn it on for any MIDI output and it follows the transport and tempo
- **Mixer.** Each track has independent **Main**, **B1**, and **B2** routing switches. The **B1 LVL** and **B2 LVL** knobs adjust the stereo post-fader sends. Main is on by default. Bus 1 has a bypassable reverb and can feed Main directly or pass through Bus 2. Both buses have their own inserts, faders, and stereo meters. Bus 2 always feeds Main.
- **MIDI tracks.** Right-click a track's name to make it a MIDI track (while it has no clips). Pick its instrument (any VST3/AU instrument) where the input selector was. Arm it and record from any MIDI keyboard; new takes replace what they're recorded over, like audio. Double-click an empty spot on a MIDI track to add a one-bar clip, and double-click a MIDI clip to edit its notes in the piano roll (click to add, drag to move, drag the end to resize, double-click to delete; use the grid selector for 1/2 through 1/32 notes or hold Cmd for free placement). MIDI clips move, trim, split and delete like audio clips.
- **Piano roll editing.** Right-click a note for Copy, Paste, Delete and Quantize. Right-drag to select several notes, or hold Shift to add to the selection. Paste preserves note spacing and velocity. **Q** quantizes selected note starts to the current grid. The lower lane shows **Velocity** or a MIDI **CC**. Drag velocity bars to adjust dynamics. Click or drag to edit CC points, and right-click a point to delete it.
- **Punch and loop.** The Punch button (P) steps through Off, Punch and Loop; drag on the lower strip of the ruler to set the range. Punch records only inside it. Loop repeats it, and recording in a loop works like a looper: start from stop and recording begins at the loop start (start while playing and it waits for the next pass); each pass is retained and the last complete pass is selected; press R again and recording stops at the end of the pass while the loop keeps playing, so you can arm the next track and add another layer. MIDI tracks overdub each pass into the same clip.
- **Audition takes.** Select a loop-recorded audio clip and click the left or right half of **Take n of N** to step through performances at the same timeline position. On narrow clips, right-click and use **Choose take**. Save As copies every alternative.
- **Song sections.** Draw a range in the punch strip, then click the section lane below it to name the range. Click a named section to locate its start. Right-click for **Duplicate**, **Remove section label**, or **Name selected range**. Duplicating inserts the whole range across tracks, including rests, MIDI, and automation. Undo restores the arrangement in one step.
- **Songwriting.** Click **Show songwriting** beside the section lane to show the Chords and Lyrics rows. Double-click a beat to add an entry or double-click an existing entry to edit it. Each lyric entry is one line at its beat. Lines take only the width of their text and overlapping lines stack in beat order. Drag a lyric by its handle to move it to another beat. Right-click to add, edit, remove, or copy a line. Paste clipboard text at a beat to add a new line. Lines sharing a beat can also move up or down or merge with the line above or below. Entries follow the tempo, save with the project, and copy with duplicated sections. Switch **Preview** on to hear a short tone when you click a supported chord symbol. Drag a playable chord symbol onto an empty spot on a MIDI track to create a MIDI clip. Dropping it onto an empty audio track asks to convert that track to MIDI first. The clip holds the chord until the next symbol, or for one bar if it is the last symbol. Collapse the view to show just the section labels.
- **Lyric Editor.** Open the editor below the chord preview button. Write one line per line break and use blank lines to separate stanzas. Drag a line by its handle to copy it onto a beat in the Lyrics lane. **Import Timeline** collects all timeline lyrics in beat order and offers to append them to the draft or replace it. The draft saves with the project. **Export Text** writes a UTF-8 lyric sheet with the project title and the entire draft.
- **Keep what I just played.** Select and arm an audio or MIDI track. When its input has buffered a performance, the command appears beneath the project name and is enabled in the Transport menu. It recovers recent input onto that track and can be undone.
- **MIDI devices.** **File > MIDI Settings...** lists the MIDI inputs (on/off, with a light that flashes on incoming notes) and outputs (on/off, MIDI clock, MMC, MIDI timecode, and a delay in ms to line a device up with the audio). Devices show up as they're plugged in. Each MIDI track's instrument button picks its **MIDI input** (all inputs or one device), **channel** (any, or 1-16), and **MIDI output**: send the track's notes to a hardware synth instead of the instrument, or as well as it (**Instrument Plays Too**). Routing is saved with the project.
- **MIDI assign.** Every plugin window has a **MIDI Learn** strip at the top. Click it, touch a control in the plugin, then move a knob or fader on your MIDI controller: that controller now moves the control. Learn stays on for the next one until you click it off. The **MIDI** button lists the plugin's assignments; click one to remove it. A controller moves one parameter, assignments are saved with the project, and each change is an undo step.
- **Automation.** Click a track's **A** button and pick volume, pan, either bus send level, EQ, or a parameter of the track's instrument or plugins. Its curve is drawn over the track's clips; click to add a point, drag to move it (snaps to the grid; hold Cmd for free), double-click to delete, right-click to clear or show the clips again. Faders and knobs follow the automation during playback, and the export includes it.
- **Plugins (VST3 and AU).** Each track, stereo bus, and Main has four pre-fader insert slots. Click **+** to add a plugin, click its slot to open the editor, or right-click to bypass or remove it. **File > Plugins...** scans in a separate process.
- **8track's own effects.** The **+** menu lists them above the plugins: **Gate** (choose a **Key Source** to let another track or bus open it, e.g. a drum track rhythmically gating a pad), **Compressor** (its **Key Source** can be any other track or a bus, so a kick can duck a bass, even a muted one), **Limiter** (with true-peak detection, for Main), **Drive** (overdrive, distortion, fuzz and tube, with four filter-modelled cabinets, nine Nave cabinet IRs, or a user impulse response WAV), **Delay**, **Chorus** (CE-2, Juno I, II and I+II, and Dimension 1-4 styles), **Reverb** (room, hall, plate, chamber, cathedral and ambience spaces with pre-delay, size, decay, diffusion, tone, modulation and width, plus **Shimmer**, which pitches the tail up or down an interval as it repeats), **Auto Pan**, **Filter** and **Pitch Shifter**. Their windows dim controls the current settings don't use. Imported impulse responses and samples are copied into the project.
- **8track's own instruments.** A MIDI track's instrument menu lists them above the plugins: **Analog Synth** (a Juno-style polysynth with saw, pulse/PWM, sub and noise, the high-pass switch, resonant filter, envelope, LFO and chorus, which in **Mono** plays SH-101-style leads and basses with glide), **Drum Sampler** (16 pads played from C2 up (MIDI notes 48–63). A new Drum Sampler loads Acoustic. Choose Electronic / Dance or Empty Kit from the kit menu, or drop your own sounds on individual pads. Each pad has live Pitch and independent Bus 1 and Bus 2 sends. Pad sends follow pad level and pan and tap the sampler before the track inserts and fader) and **Sampler** (plays one sound up the keyboard: load a file, or pick an input and press **Record**; **Find Root** hears the note it was played at). On a MIDI track the **+** menu also offers the **Arpeggiator** (up, down, up/down, random, as played or chords, in time with the song, with octaves, gate, swing and latch), which goes ahead of the instrument.
- **Drum Patterns.** Open **Patterns** in the Drum Sampler to program one of eight slots from 4 to 64 beats in sixteenth-note steps. Set the length in beats and use the step-range menu to move between pages. Click **Preview** while the song is stopped to loop the slot through the current kit at the project tempo. Switch to **Pads** during preview to shape the kit, and use **Stop Preview** there when finished. Drag a filled slot onto a MIDI track to place an editable MIDI clip. With a Drum Sampler track selected, **Drums** in the toolbar opens its Patterns tab. Slots save with the project, and each drop makes an independent copy.
- **Projects** are folders (`project.8track` + `Audio/`). Opening one at a different sample rate converts it. Projects saved by 4track (the earlier name) still open.
- **File > Export Audio...** (Cmd+E) renders the mix offline, including the reverb tail. Choose WAV (16-bit or 24-bit PCM, or 32-bit float) or MP3 (64–320 kbps), then choose where to save it.

## Keyboard

Open **Help > Keyboard Shortcuts** with **F1** or **Cmd+/** to search the current shortcuts. Click the arrangement, piano roll, or a songwriting lane to give it keyboard focus. Text fields keep their normal editing keys.

In the **piano roll**, Left/Right moves selected notes by a grid step, Up/Down transposes by a semitone, and Shift+Up/Down transposes by an octave. Cmd+D duplicates the selected notes, and Q quantizes their starts. In **songwriting**, Left/Right moves the lane cursor by a beat, Return adds or edits an entry, and Delete removes the selected chord or lyric. Cmd+X/C/V uses the text clipboard, and Esc returns focus to the arrangement.

### Arrangement and transport

| Key | Action | Key | Action |
|---|---|---|---|
| Space | Play / stop (stop returns to where you started) | R | Record / stop recording |
| Return | Go to start | ← / → | Move playhead by grid step (Shift: by bar) |
| S | Split selected clips at the playhead | Delete | Delete selected clips |
| P / L | Range off → punch → loop / loop on-off | I / O | Set range start / end at the playhead |
| C | Metronome | X | Show/hide mixer |
| = / - / Z | Zoom in / out / to fit | Cmd+Z / Shift+Cmd+Z | Undo / redo |
| Cmd+S / Shift+Cmd+S | Save / Save As | Cmd+O / Cmd+N | Open / New |
| Cmd+I / Cmd+E | Import audio / Export audio | Cmd+, | Audio settings |
| Cmd+X / Cmd+C | Cut / copy selected clips | Cmd+V | Paste at the playhead, on the selected track |
| Cmd+D | Duplicate selected clips straight after themselves | | |

Mouse: drag a clip to move it (to other tracks too), drag its edges to trim, drag its top corners to fade, and right-click for more. Drag on empty space to draw a rectangle: it selects every clip it touches (Shift adds to the selection), and right-click a selected clip for **Join Clips** (each track's selected clips become one clip; audio is rendered with its gain and fades into a new file, MIDI notes are merged), **Normalise** (sets each audio clip's gain so it peaks at 0 dB; the file isn't touched), **Transpose** (MIDI: ±1 semitone or octave), and cut / copy / duplicate / delete. These are in the clip right-click menu too. Hold Cmd while dragging to bypass snap. Cmd+scroll or pinch to zoom. Drop audio files onto a track to import them.

## Building

Requires CMake 3.22+ and clang 16 or newer: Tracktion Engine uses C++20 features that Apple clang 15
(Xcode 15) can't compile. Xcode 16, or Homebrew's LLVM (`brew install llvm`), works. Tracktion Engine
is a git submodule and brings the JUCE version it needs (9.0.5):

```bash
git submodule update --init tracktion_engine
# Tracktion's JUCE submodule uses an SSH URL; this fetches it over HTTPS instead:
git -C tracktion_engine config submodule.modules/juce.url https://github.com/juce-framework/JUCE.git
git -C tracktion_engine submodule update --init modules/juce
# 8track's changes to Tracktion and JUCE (input observers, plugin windows, and more):
git -C tracktion_engine apply ../patches/tracktion_engine.patch
git -C tracktion_engine/modules/juce apply ../../../patches/juce.patch
# With Homebrew LLVM on an Intel Mac (use /opt/homebrew/opt/llvm on Apple silicon):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=/usr/local/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm/bin/clang++
cmake --build build
open build/8Track_artefacts/Debug/8track.app
```

Install LAME before configuring CMake to enable MP3 export (`brew install lame` on macOS). CMake looks for the LAME headers and library. WAV export is also available in builds without LAME.

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
(GPLv3). Each mixer channel uses Tracktion plugins for EQ, inserts, the fader, and stereo sends.
Two dedicated tracks handle the stereo bus returns. Clip edits are made on an `Arrangement` copy
and written back to the Edit as one undo step.

## Third-party code

- [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://juce.com) (GPLv3)
- [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, shipped with Tracktion): the Pitch Shifter and the Reverb's shimmer
- [Ultramaster KR-106](https://github.com/kayrockscreenprinting/ultramaster_kr106) (GPLv3, in `ThirdParty/kr106`): the Analog Synth's voices and the Chorus's Juno modes
- [Factory drum kits](ThirdParty/factory-drum-kits/README.8track.md) from Virtuosity Drums and Fugue State Audio (CC0 1.0)
- [Nave cabinet IRs](ThirdParty/nave-cabinet-irs/README.8track.md) by Basilica Audio (CC0 1.0, in `ThirdParty/nave-cabinet-irs`): the modelled guitar and bass cabinet responses in Drive
- [CloudSeedCore](https://github.com/GhostNoteAudio/CloudSeedCore) by Ghost Note Audio (MIT, in `ThirdParty/cloudseed`): the Reverb's engine

Each folder in `ThirdParty` keeps its licence and a note of the version taken.
