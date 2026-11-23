#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include "Parameters.h"
#include "../Model/Arrangement.h"
#include <array>
#include <functional>
#include <map>
#include <memory>

namespace te = tracktion;

/**
    The audio side of 8track, built on Tracktion Engine.

    Tracktion owns the audio device, the transport, recording (punch, count-in,
    click, latency compensation) and the mixer, which is a chain of Tracktion
    plugins on each track:

        high-pass filter > EQ (mid band) > low-pass filter > volume & pan > reverb send > meter

    A hidden ninth track returns the reverb send through a reverb plugin.

    The UI talks to this class through the same small surface as before: atomics
    for tempo, punch and click, and TrackParams/MasterParams for each strip. A
    message-thread timer keeps those in step with the Edit in both directions (a
    change on either side wins), and drains Tracktion's level meters into the
    params' peak values.
*/
class AudioEngine : public juce::ChangeBroadcaster,
                    private juce::ChangeListener,
                    private te::TransportControl::Listener,
                    private juce::Timer
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

    // Mixer
    TrackParams& getTrack(int index) { return tracks[(size_t) index]; }
    MasterParams& getMaster() { return master; }

    // Plugin inserts: VST3/AU effects on a track, after the EQ and before the fader.
    // kMasterBus in place of a track means the master's inserts: on the whole mix
    // (reverb included), before the master fader.
    static constexpr int kMaxInserts = 4;
    static constexpr int kMasterBus = -1;
    juce::KnownPluginList& getKnownPlugins() { return engine.getPluginManager().knownPluginList; }
    /** The track's inserts in signal order. */
    std::vector<te::Plugin*> getInserts(int track) const;
    /** Loads a plugin and adds it after the track's other inserts, as one undo step. */
    te::Plugin* addInsert(int track, const juce::PluginDescription&, juce::String& error);
    /** Loads a plugin, or explains why it couldn't be. */
    te::Plugin::Ptr loadPlugin(const juce::PluginDescription&, juce::String& error);
    void removeInsert(te::Plugin&);

    // MIDI tracks: TrackParams::midi switches a track to MIDI (only do that while it has no
    // clips). A MIDI track records from every MIDI input and plays through its instrument,
    // which sits at the start of the track's chain, before the EQ.
    te::Plugin* getInstrument(int track) const;
    /** Loads an instrument plugin and makes it the track's instrument (replacing any other), as one undo step. */
    te::Plugin* setInstrument(int track, const juce::PluginDescription&, juce::String& error);
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

    // Tempo, metronome, punch (synced to the Edit by the timer)
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> metronomeOn { false };
    std::atomic<bool> countInOn { true };
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

    /** Of a loop recording's takes, keeps the last full pass as the clip's audio (or the first,
        if none is complete) and drops the takes list. Called after each loop recording. */
    void keepLastFullPass(te::WaveAudioClip&);

    double getSamplesPerBeat() const { return getSampleRate() * 60.0 / bpm.load(); }
    juce::int64 getSamplesPerBar() const { return (juce::int64) (getSamplesPerBeat() * beatsPerBar.load()); }

    // Transport (message thread)
    State getState() const;
    juce::int64 getPosition() const;
    void setPosition(juce::int64 newPosition);
    void play();
    void stop();
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

    static constexpr int kReverbTrack = kNumTracks;   // index of the hidden reverb return track

private:
    /** The plugins that make up one channel strip. */
    struct Strip
    {
        te::AudioTrack* track = nullptr;
        te::LowPassPlugin* lowCut = nullptr;
        te::EqualiserPlugin* eq = nullptr;
        te::LowPassPlugin* highCut = nullptr;
        te::VolumeAndPanPlugin* volume = nullptr;
        te::AuxSendPlugin* send = nullptr;
        te::LevelMeasurer::Client meter;
        te::LevelMeasurer* measurer = nullptr;
    };

    /** Last values exchanged with the Edit, to tell which side changed. */
    struct Synced
    {
        float volumeDb = 0, pan = 0, send = 0, lowCut = 20, highCut = 20000, midFreq = 1000, midGainDb = 0;
        bool mute = false, solo = false, midi = false;
        juce::String name;
        juce::uint32 colour = 0;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void buildStrips();
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

    te::Engine engine;
    te::Edit* edit = nullptr;

    std::array<TrackParams, kNumTracks> tracks;
    std::array<Strip, kNumTracks> strips;
    std::array<Synced, kNumTracks> synced;
    MasterParams master;
    te::ReverbPlugin* reverb = nullptr;
    te::LevelMeasurer::Client masterMeter;
    te::LevelMeasurer* masterMeasurer = nullptr;
    te::EditPlaybackContext* meteredContext = nullptr;   // owner of masterMeasurer: Tracktion replaces it (e.g. around a render)
    struct { float volumeDb = 0, room = 0.5f, damping = 0.5f, wet = 0.33f; } syncedMaster;
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
    juce::int64 lastLoopPosition = 0;
    double lastSampleRate = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
