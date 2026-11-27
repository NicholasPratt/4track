#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include "Parameters.h"
#include "MidiOut.h"
#include "ChordPreview.h"
#include "../Model/Arrangement.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace te = tracktion;
class MainOutputGate;

/**
    The audio side of 8track, built on Tracktion Engine.

    Tracktion owns the audio device, the transport, recording (punch, count-in,
    click, latency compensation) and the mixer, which is a chain of Tracktion
    plugins on each track:

        high-pass filter > EQ (mid band) > low-pass filter > inserts > volume & pan > bus sends

    Two stereo bus tracks sum post-fader sends. Bus 1 has reverb and can feed Bus 2 or Main.

    The UI talks to this class through the same small surface as before: atomics
    for tempo, punch and click, and mixer parameters for each strip. A
    message-thread timer keeps those in step with the Edit in both directions (a
    change on either side wins), and drains Tracktion's level meters into the
    params' peak values.
*/
class AudioEngine : public juce::ChangeBroadcaster,
                    private juce::ChangeListener,
                    private te::TransportControl::Listener,
                    private juce::Timer,
                    private juce::AsyncUpdater
{
public:
    enum class State { Stopped = 0, Playing = 1, Recording = 2 };

    /** A take being recorded right now, for drawing it live. */
    struct LiveTake
    {
        int track = 0;
        juce::File file;
        juce::int64 start = 0;   // timeline position of the first recorded sample
    };

    /** The UI passes in a UIBehaviour that opens plugin editor windows; tests pass none. */
    explicit AudioEngine(std::unique_ptr<te::UIBehaviour> ui = {});
    ~AudioEngine() override;

    te::Engine& getTracktion() { return engine; }

    // Device
    juce::AudioDeviceManager& getDeviceManager() { return engine.getDeviceManager().deviceManager; }
    double getSampleRate() const;
    bool isChordPreviewEnabled() const { return chordPreview.isEnabled(); }
    void setChordPreviewEnabled(bool enabled) { chordPreview.setEnabled(getDeviceManager(), enabled); }
    void previewChord(const juce::String& chord)
    {
        if (getState() != State::Recording && getLoopRecord() != LoopRecord::Waiting)
            chordPreview.play(chord);
    }
    /** An input a track can record from: one hardware channel, or a stereo pair. Tracks
        store the input's first channel (TrackParams::inputChannel). */
    struct Input
    {
        int channel = 0;        // first hardware channel, 0-based
        int numChannels = 1;
        juce::String name;      // e.g. "In 3" or "In 1/2"
        juce::String label;     // the interface's name for it, e.g. "Loop 1" or "Input 1 + 2"
    };
    std::vector<Input> getInputs() const;
    struct ArmedInputChannel { int channel; juce::String label; };
    std::vector<ArmedInputChannel> getArmedInputChannels() const;

    /** Stereo pairs (1/2, 3/4, ...) are a device setting shared by all tracks, made in Audio
        Settings. firstChannel is the left channel (0, 2, 4, ...). */
    bool canPairInputs(int firstChannel) const;
    bool isInputPaired(int firstChannel) const;
    void setInputPaired(int firstChannel, bool stereo);

    /** The Edit being played. Set by Project whenever it creates or loads one (nullptr
        before that Edit is deleted). A fresh Edit gets 8track's default track names,
        colours and count-in. */
    void attachEdit(te::Edit* edit, bool freshEdit = false);
    te::Edit* getEdit() const { return edit; }
    te::AudioTrack* getAudioTrack(int index) const;
    void setTrackMidi(int index, bool midi);
    bool isTrackMidi(int index) const { return juce::isPositiveAndBelow(index, kNumTracks) && tracks[(size_t) index].midi.load(); }

    // Mixer
    TrackParams& getTrack(int index) { return tracks[(size_t) index]; }
    OutputParams& getMaster() { return master; }
    OutputParams& getBus(int bus) { return buses[(size_t) bus].params; }
    ReverbParams& getBusReverb() { return busReverb; }
    enum class Route { Main, Bus1, Bus2 };
    bool hasTrackRoute(int track, Route) const;
    void setTrackRoute(int track, Route, bool enabled);
    bool bus1FeedsBus2() const;
    void setBus1FeedsBus2(bool);
    bool isBusReverbEnabled() const;
    void setBusReverbEnabled(bool);

    // Plugin inserts: VST3/AU effects on a track, after the EQ and before the fader.
    // Bus and master indices address the inserts before the corresponding fader.
    static constexpr int kMaxInserts = 4;
    static constexpr int kMasterBus = -1;
    static constexpr int kStereoBusCount = 2;
    static constexpr int kFirstStereoBus = kNumTracks;
    juce::KnownPluginList& getKnownPlugins() { return engine.getPluginManager().knownPluginList; }
    /** The track's inserts in signal order. */
    std::vector<te::Plugin*> getInserts(int track) const;
    /** Loads a plugin and adds it after the track's other inserts, as one undo step. */
    te::Plugin* addInsert(int track, const juce::PluginDescription&, juce::String& error);
    /** Loads a plugin, or explains why it couldn't be. */
    te::Plugin::Ptr loadPlugin(const juce::PluginDescription&, juce::String& error);
    void removeInsert(te::Plugin&);
    /** Adds one of 8track's own effects (EffectPlugin::getTypes()) after the track's other
        inserts, as one undo step. */
    te::Plugin* addEffect(int track, const juce::String& effectType, juce::String& error);

    // MIDI tracks: TrackParams::midi switches a track to MIDI (only do that while it has no
    // clips). A MIDI track records from every MIDI input and plays through its instrument,
    // which sits at the start of the track's chain, before the EQ.
    te::Plugin* getInstrument(int track) const;
    /** Loads an instrument plugin and makes it the track's instrument (replacing any other), as one undo step. */
    te::Plugin* setInstrument(int track, const juce::PluginDescription&, juce::String& error);
    /** Makes one of 8track's own instruments (EffectPlugin::getTypes()) the track's instrument. */
    te::Plugin* setInstrument(int track, const juce::String& instrumentType, juce::String& error);
    void removeInstrument(int track);
    // Automation: any of a track's parameters can follow a curve. The mixer's controls come
    // first, then every parameter of the track's instrument and inserts.
    struct AutomationTarget
    {
        te::AutomatableParameter* param = nullptr;
        juce::String name;    // e.g. "Volume" or "Cutoff"
        juce::String group;   // "Mixer", or the plugin's name
        juce::String key;     // stable across undo and reloading: plugin id + "/" + parameter id
    };
    std::vector<AutomationTarget> getAutomationTargets(int track) const;
    te::AutomatableParameter* findAutomationTarget(int track, const juce::String& key) const;

    /** Names of the enabled MIDI inputs. */
    juce::StringArray getMidiInputNames() const;

    // MIDI devices. These are machine settings, saved with Tracktion's settings rather than
    // the project. Devices come and go as they're plugged in: the engine broadcasts a change.
    struct MidiInput
    {
        juce::String name;
        bool enabled = false;
        double lastActivityMs = 0;   // juce::Time::getMillisecondCounterHiRes() of the last message, 0 if none
    };
    /** The hardware (and other apps') MIDI inputs. */
    std::vector<MidiInput> getMidiInputs() const;
    void setMidiInputEnabled(int inputIndex, bool enabled);

    /** A MIDI output, and what it sends besides tracks' notes: MIDI clock (start/stop, song
        position and 24 ppq clock following the transport and tempo), MMC (transport commands)
        and MIDI timecode. delayMs delays everything sent to it, clock and notes, to line a
        slow device up with the audio (negative sends earlier). */
    struct MidiOutput
    {
        juce::String name;
        bool enabled = false;
        bool sendingClock = false;
        bool sendingMMC = false;
        bool sendingTimecode = false;
        int delayMs = 0;
    };
    std::vector<MidiOutput> getMidiOutputs();
    void setMidiOutputEnabled(int outputIndex, bool enabled);
    void setSendingMidiClock(int outputIndex, bool shouldSend);
    void setSendingMMC(int outputIndex, bool shouldSend);
    void setSendingTimecode(int outputIndex, bool shouldSend);
    void setMidiOutputDelayMs(int outputIndex, int ms);

    /** Where a MIDI track's MIDI comes from and goes to. Saved with the project. */
    struct MidiRouting
    {
        juce::String input;              // a MIDI input's name; empty = every input
        int channel = 0;                 // 1..16, or 0 = any channel
        juce::String output;             // a MIDI output to send the track's notes to; empty = none
        bool replaceInstrument = false;  // with an output: true = only the hardware plays, false = both
        bool operator==(const MidiRouting&) const = default;
    };
    MidiRouting getMidiRouting(int track) const;
    void setMidiRouting(int track, const MidiRouting&);

    // MIDI assign: a controller (CC) on any MIDI input moves a parameter: a plugin's, or a
    // mixer control. Saved with the project; each change is an undo step. A controller moves
    // one parameter, and a parameter follows one controller.
    struct MidiAssignment
    {
        juce::String key;     // the parameter, as parameterKey() gives it
        int channel = 1;      // 1..16
        int controller = 0;   // CC number, 0..127
    };
    std::vector<MidiAssignment> getMidiAssignments() const;
    std::optional<MidiAssignment> getMidiAssignment(const juce::String& key) const;
    void assignMidi(const juce::String& key, int channel, int controller);
    void removeMidiAssignment(const juce::String& key);
    /** A stable name for a parameter, kept across undo and reloading: plugin id + "/" + parameter id. */
    static juce::String parameterKey(te::AutomatableParameter&);
    /** The parameter with this key, on any track or the master bus. */
    te::AutomatableParameter* findParameter(const juce::String& key) const;
    /** While set, incoming controllers come here (on the message thread) instead of moving
        parameters: for learning an assignment. */
    std::function<void(int channel, int controller)> onMidiLearn;
    /** Moves the parameters assigned to this controller. Incoming MIDI calls it; so can tests. */
    void handleController(int channel, int controller, int value);

    // Tempo, metronome, punch (synced to the Edit by the timer)
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> metronomeOn { false };
    std::atomic<bool> countInOn { true }, playbackCountInOn { false }, metronomeRecordOnly { false };
    std::atomic<bool> punchOn { false };
    std::atomic<bool> loopOn { false };   // never both: use setRangeMode
    std::atomic<juce::int64> punchIn { 0 }, punchOut { 0 };   // the range punch and loop both use

    /** What the range does: nothing, limits recording to it (punch), or repeats it (loop). */
    enum class RangeMode { Off, Punch, Loop };
    RangeMode getRangeMode() const { return loopOn.load() ? RangeMode::Loop : punchOn.load() ? RangeMode::Punch : RangeMode::Off; }
    void setRangeMode(RangeMode);
    bool hasRange() const { return punchOut.load() > punchIn.load(); }
    /** Off -> Punch -> Loop -> Off: what the Punch/Loop button steps through. */
    static RangeMode nextRangeMode(RangeMode m) { return m == RangeMode::Off ? RangeMode::Punch : m == RangeMode::Punch ? RangeMode::Loop : RangeMode::Off; }

    /** Recording in loop mode works like a looper. Started from stop, recording begins at the
        start of the loop; started while playing, at the start of the next pass. Each pass
        records a take, and the last complete pass is kept. Record again to finish: recording
        stops at the end of the current pass and playback carries on, ready for the next layer
        (on another track). MIDI passes are merged: each pass overdubs the one before. */
    enum class LoopRecord { Off, Waiting, Recording, Finishing };
    LoopRecord getLoopRecord() const { return loopRecord; }
    static constexpr double kMinLoopSeconds = 2.0;   // Tracktion's shortest loop for recording

    inline static const juce::Identifier takeListID { "eightTrackTakes" };
    inline static const juce::Identifier takePathID { "takeFile" };
    void storeTakes(te::WaveAudioClip&, const juce::Array<juce::File>&);
    void keepLastFullPass(te::WaveAudioClip&);

    double getSamplesPerBeat() const { return getSampleRate() * 60.0 / bpm.load(); }
    juce::int64 getSamplesPerBar() const { return (juce::int64) (getSamplesPerBeat() * beatsPerBar.load()); }

    // Transport (message thread)
    State getState() const;
    juce::int64 getPosition() const;
    void setPosition(juce::int64 newPosition);
    void play();
    void stop();
    /** MIDI panic: every note off, on every instrument (8track's and plugins) and every
        hardware MIDI output, and the sustain pedal up. Message thread. */
    void allNotesOff();
    /** Starts recording on the armed tracks. New takes are written into takeFolder.
        In loop mode this also moves the looper on: see LoopRecord. */
    bool record(const juce::File& takeFolder, juce::String& error);
    std::vector<LiveTake> getLiveTakes() const;

    /** Writes any pending UI changes into the Edit now (before saving or rendering). */
    void syncNow() { sync(); }

    juce::int64 toSamples(te::TimePosition t) const { return (juce::int64) std::llround(t.inSeconds() * getSampleRate()); }
    juce::int64 toSamples(te::TimeDuration d) const { return (juce::int64) std::llround(d.inSeconds() * getSampleRate()); }
    te::TimePosition toTime(juce::int64 samples) const { return te::TimePosition::fromSeconds((double) samples / getSampleRate()); }
    te::TimeDuration toDuration(juce::int64 samples) const { return te::TimeDuration::fromSeconds((double) samples / getSampleRate()); }

private:
    /** The plugins that make up one channel strip. */
    struct Strip
    {
        te::AudioTrack* track = nullptr;
        te::LowPassPlugin* lowCut = nullptr;
        te::EqualiserPlugin* eq = nullptr;
        te::LowPassPlugin* highCut = nullptr;
        te::VolumeAndPanPlugin* volume = nullptr;
        std::array<te::AuxSendPlugin*, kStereoBusCount> sends {};
        MainOutputGate* mainGate = nullptr;
        te::LevelMeasurer::Client meter;
        te::LevelMeasurer* measurer = nullptr;
    };

    /** Last values exchanged with the Edit, to tell which side changed. */
    struct Synced
    {
        float volumeDb = 0, pan = 0, lowCut = 20, highCut = 20000, midFreq = 1000, midGainDb = 0;
        std::array<float, kStereoBusCount> sendLevels { 1.0f, 1.0f };
        bool mute = false, solo = false, midi = false;
        juce::String name;
        juce::uint32 colour = 0;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void buildStrips();
    bool canAddInsert(int track, juce::String& error) const;
    te::Plugin* placeInsert(int track, te::Plugin::Ptr);
    te::Plugin* placeInstrument(int track, te::Plugin::Ptr);
    int midiEffectsEnd(int track) const;   // index after the MIDI effects at the head of a track's chain
    void updateEffectTempo();   // tells 8track's effects the song tempo
    void attachMeters();
    void detachMeters();
    void pullAll();
    void sync();
    void syncTrack(int index);
    void syncMaster();
    void syncTransportSettings();
    void applyInputs();
    void drainMeters();
    te::WaveInputDevice* inputForChannel(int channel) const;
    static int firstChannelOf(const te::WaveInputDevice&);
    void setUpInputLayout();
    void normaliseInputChoices();

    // MIDI
    te::MidiInputDevice* trackMidiInput(int track) const;   // the virtual input a MIDI track records from
    void ensureTrackMidiInputs();
    void configureTrackMidiInput(int track);
    MidiOutPlugin* getMidiOutPlugin(int track) const;
    void connectMidiOutPlugins();
    void updateMidiOutLatency();
    void refreshMidiActivity();
    te::MidiOutputDevice* midiOutput(int index) const;
    std::vector<std::shared_ptr<te::MidiInputDevice>> physicalMidiInputs() const;

    MidiOutSender midiOutSender;

    // Controllers from the MIDI threads, applied on the message thread.
    void handleAsyncUpdate() override;
    void queueController(int channel, int controller, int value);
    rigtorp::MPMCQueue<std::array<int, 3>> controllerQueue { 4096 };

    struct MidiActivity : te::PhysicalMidiInputDevice::Listener
    {
        AudioEngine* owner = nullptr;
        std::weak_ptr<te::MidiInputDevice> device;   // Tracktion replaces its devices when it rescans
        juce::String name;
        std::atomic<double> lastMs { 0 };
        void handleIncomingMidiMessage(const juce::MidiMessage& m) override
        {
            if (!m.isActiveSense() && !m.isMidiClock())
                lastMs = juce::Time::getMillisecondCounterHiRes();
            if (m.isController())
                owner->queueController(m.getChannel(), m.getControllerNumber(), m.getControllerValue());
        }
    };
    std::vector<std::unique_ptr<MidiActivity>> midiActivity;

    te::Engine engine;
    ChordPreview chordPreview;
    te::Edit* edit = nullptr;

    std::array<TrackParams, kNumTracks> tracks;
    std::array<Strip, kNumTracks> strips;
    std::array<Synced, kNumTracks> synced;
    struct StereoBus
    {
        te::AudioTrack* track = nullptr;
        te::VolumeAndPanPlugin* volume = nullptr;
        te::AuxReturnPlugin* input = nullptr;
        OutputParams params;
        float syncedVolumeDb = 0.0f;
        te::LevelMeasurer::Client meter;
        te::LevelMeasurer* measurer = nullptr;
    };
    std::array<StereoBus, kStereoBusCount> buses;
    OutputParams master;
    ReverbParams busReverb;
    te::ReverbPlugin* reverb = nullptr;
    te::LevelMeasurer::Client masterMeter;
    te::LevelMeasurer* masterMeasurer = nullptr;
    te::EditPlaybackContext* meteredContext = nullptr;   // owner of masterMeasurer: Tracktion replaces it (e.g. around a render)
    float syncedMasterVolumeDb = 0.0f;
    struct { float room = 0.5f, damping = 0.5f, wet = 0.33f; } syncedReverb;
    // Input meters for armed tracks, keyed by the Tracktion input they listen to.
    std::map<te::WaveInputDevice*, std::unique_ptr<te::LevelMeasurer::Client>> inputMeters;

    struct InputState { bool armed = false; int monitor = 0, type = 0, channel = 0; };
    std::array<InputState, kNumTracks> syncedInputs;
    bool inputsDirty = true;

    // Transport settings last written to the Edit
    double syncedBpm = 0;
    int syncedBeatsPerBar = 0;
    bool syncedClick = false, syncedPunchOn = false, syncedLoopOn = false;
    juce::int64 syncedPunchIn = -1, syncedPunchOut = -1;

    juce::int64 returnPosition = 0;   // where stop() sends the playhead

    // Looper: watches for the playhead wrapping to the loop start, often, while the looper
    // is waiting to start or finish.
    void checkLoopWrap();
    void recordingFinished(te::InputDeviceInstance&, te::EditItemID, const juce::ReferenceCountedArray<te::Clip>&) override;
    struct LoopWatcher : juce::Timer
    {
        std::function<void()> tick;
        void timerCallback() override { tick(); }
    } loopWatcher;
    LoopRecord loopRecord = LoopRecord::Off;
    juce::int64 loopPassOffset() const;
    juce::int64 armedLoopOffset = 0;
    double lastSampleRate = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
