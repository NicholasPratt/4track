#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MixEngine.h"
#include <functional>
#include <memory>

/**
    Owns the audio device, the transport and recording.

    Threading:
      - The audio callback never allocates, locks or touches the disk. Recorded input
        goes to a juce::AudioFormatWriter::ThreadedWriter, which hands it to a
        background thread for writing.
      - The arrangement reaches the audio thread as an immutable snapshot. The
        snapshot pointer is swapped while holding the device's callback lock, so the
        old one is always freed on the message thread.
      - Mixer settings are atomics in TrackParams/MasterParams.

    Recording:
      - Capture is sample-accurate in the callback: the punch range, count-in and
        pre-roll are worked out per sample, not by a UI timer.
      - Input is latency-compensated: what reaches the callback at time t was played
        against timeline position t - (input + output latency), so the capture window
        is shifted by that amount and each take lands where it was performed.
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::ChangeBroadcaster,
                    private juce::ChangeListener,
                    private juce::AsyncUpdater
{
public:
    enum class State { Stopped = 0, Playing = 1, Recording = 2 };

    struct FinishedTake
    {
        int track = 0;
        AudioSourcePtr source;
        juce::int64 start = 0;
        juce::int64 length = 0;
    };

    /** A take being recorded right now, readable from the message thread for live waveforms. */
    struct LiveTake
    {
        int track = 0;
        InputType inputType = InputType::Mono;
        int channel = 0;
        juce::File file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        std::vector<float> peakMin, peakMax;   // per AudioSourceData::kPeakBlock samples
        std::atomic<juce::int64> written { 0 };
        std::atomic<bool> overflowed { false };
    };

    struct RecordSession
    {
        std::vector<std::unique_ptr<LiveTake>> takes;
        juce::int64 windowStart = 0, windowEnd = 0;     // timeline positions
        juce::int64 captureStart = 0, captureEnd = 0;   // callback positions (window + latency)
        juce::int64 latency = 0;
        juce::int64 maxSamples = 0;
        bool punch = false;
        // Callback position of the first captured sample (set by the audio thread)
        std::atomic<juce::int64> firstCapture { std::numeric_limits<juce::int64>::min() };
    };

    /** openDevice = false gives an engine with no audio device, driven by calling the callback directly (tests). */
    explicit AudioEngine(bool openDevice = true);
    ~AudioEngine() override;

    // Device
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    double getSampleRate() const { return sampleRate.load(); }
    juce::StringArray getActiveInputNames() const;
    juce::int64 getRoundTripLatency() const;

    // Arrangement snapshot played by the audio thread
    void setArrangement(std::shared_ptr<const Arrangement> snapshot);

    // Mixer
    TrackParams& getTrack(int index) { return tracks[(size_t) index]; }
    MasterParams& getMaster() { return master; }
    const MixEngine::ParamArray& getParamArray() const { return paramArray; }

    // Tempo, metronome, punch (set from the UI, read by the audio thread)
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> metronomeOn { false };
    std::atomic<bool> countInOn { true };
    std::atomic<float> clickLevelDb { -6.0f };
    std::atomic<bool> punchOn { false };
    std::atomic<juce::int64> punchIn { 0 }, punchOut { 0 };

    double getSamplesPerBeat() const { return getSampleRate() * 60.0 / bpm.load(); }
    juce::int64 getSamplesPerBar() const { return (juce::int64) (getSamplesPerBeat() * beatsPerBar.load()); }

    // Transport (message thread)
    State getState() const { return (State) state.load(); }
    juce::int64 getPosition() const;
    void setPosition(juce::int64 newPosition);
    void play();
    void stop();
    /** Starts recording on the armed tracks. Takes are written into takeFolder. */
    bool record(const juce::File& takeFolder, juce::String& error);

    /** Called on the message thread after stop() with the takes that were captured. */
    std::function<void(std::vector<FinishedTake>)> onTakesFinished;

    const RecordSession* getRecordSession() const { return session.get(); }

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override;

    void processChunk(const float* const* inputs, int numInputs, float* outL, float* outR, int n);
    void captureInput(const float* const* inputs, int numInputs, const float* mixL, const float* mixR, int n);
    void addMetronome(float* outL, float* outR, int n, juce::int64 position, bool countInClick);

    void initialiseDevice();
    void saveDeviceState();
    std::vector<FinishedTake> finishSession(std::unique_ptr<RecordSession> finished);

    juce::AudioDeviceManager deviceManager;
    juce::TimeSliceThread diskThread { "8track disk writer" };

    std::array<TrackParams, kNumTracks> tracks;
    MasterParams master;
    MixEngine::ParamArray paramArray {};
    MixEngine mixer;

    std::atomic<double> sampleRate { 44100.0 };
    int scratchSize = 0;
    juce::AudioBuffer<float> scratch;    // ch 0/1: master mix, ch 2: silence
    std::array<MixEngine::TrackFeed, kNumTracks> feeds {};

    // Transport
    std::atomic<int> state { (int) State::Stopped };
    std::atomic<juce::int64> position { 0 };
    static constexpr juce::int64 kNoLocate = std::numeric_limits<juce::int64>::min();
    std::atomic<juce::int64> pendingLocate { kNoLocate };
    juce::int64 returnPosition = 0;      // where stop() sends the playhead

    // Arrangement snapshot: replaced under the callback lock
    std::shared_ptr<const Arrangement> audioArrangement;

    // Recording: owned on the message thread, read by the audio thread while set
    std::unique_ptr<RecordSession> session;
    RecordSession* audioSession = nullptr;
    int takeCounter = 0;

    // Metronome voice (audio thread only)
    int clickRemaining = 0;
    double clickPhase = 0.0, clickPhaseInc = 0.0;
    float clickAmp = 0.0f;

    bool deviceInitialised = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
