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
    /** One name per input channel, in channel order. Tracks pick an input by index. */
    juce::StringArray getActiveInputNames() const;

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
    static constexpr int kMaxInserts = 4;
    juce::KnownPluginList& getKnownPlugins() { return engine.getPluginManager().knownPluginList; }
    /** The track's inserts in signal order. */
    std::vector<te::Plugin*> getInserts(int track) const;
    /** Loads a plugin and adds it after the track's other inserts, as one undo step. */
    te::Plugin* addInsert(int track, const juce::PluginDescription&, juce::String& error);
    void removeInsert(te::Plugin&);

    // Tempo, metronome, punch (synced to the Edit by the timer)
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> metronomeOn { false };
    std::atomic<bool> countInOn { true };
    std::atomic<bool> punchOn { false };
    std::atomic<juce::int64> punchIn { 0 }, punchOut { 0 };

    double getSamplesPerBeat() const { return getSampleRate() * 60.0 / bpm.load(); }
    juce::int64 getSamplesPerBar() const { return (juce::int64) (getSamplesPerBeat() * beatsPerBar.load()); }

    // Transport (message thread)
    State getState() const;
    juce::int64 getPosition() const;
    void setPosition(juce::int64 newPosition);
    void play();
    void stop();
    /** Starts recording on the armed tracks. New takes are written into takeFolder. */
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
        bool mute = false, solo = false;
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

    te::Engine engine;
    te::Edit* edit = nullptr;

    std::array<TrackParams, kNumTracks> tracks;
    std::array<Strip, kNumTracks> strips;
    std::array<Synced, kNumTracks> synced;
    MasterParams master;
    te::ReverbPlugin* reverb = nullptr;
    te::LevelMeasurer::Client masterMeter;
    te::LevelMeasurer* masterMeasurer = nullptr;
    struct { float volumeDb = 0, room = 0.5f, damping = 0.5f, wet = 0.33f; } syncedMaster;
    // Input meters for armed tracks, keyed by the Tracktion input they listen to.
    std::map<te::WaveInputDevice*, std::unique_ptr<te::LevelMeasurer::Client>> inputMeters;

    struct InputState { bool armed = false; int monitor = 0, type = 0, channel = 0; };
    std::array<InputState, kNumTracks> syncedInputs;
    bool inputsDirty = true;

    // Transport settings last written to the Edit
    double syncedBpm = 0;
    int syncedBeatsPerBar = 0;
    bool syncedClick = false, syncedPunchOn = false;
    juce::int64 syncedPunchIn = -1, syncedPunchOut = -1;

    juce::int64 returnPosition = 0;   // where stop() sends the playhead
    double lastSampleRate = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
