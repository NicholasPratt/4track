#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

class Track
{
public:
    enum class InputMode { Mono, Stereo, BounceFromMix };

    Track(int trackNumber);
    ~Track();

    // Audio processing
    void processBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples);
    void addInputSample(float leftSample, float rightSample, int position);
    void addToReverbSend(juce::AudioBuffer<float>& sendBuffer, int startSample, int numSamples, float sendLevel);

    // Playback control
    void setPlaybackPosition(int position) { playbackPosition = position; }
    int getPlaybackPosition() const { return playbackPosition; }

    // Track state
    void setRecordArmed(bool shouldArm) { recordArmed = shouldArm; }
    bool isRecordArmed() const { return recordArmed; }

    void setMuted(bool shouldMute) { muted = shouldMute; }
    bool isMuted() const { return muted; }

    void setSolo(bool shouldSolo) { solo = shouldSolo; }
    bool isSolo() const { return solo; }

    // Volume and pan (0.0 to 1.0 for volume, -1.0 to 1.0 for pan)
    void setVolume(float newVolume) { volume = juce::jlimit(0.0f, 1.0f, newVolume); }
    float getVolume() const { return volume; }

    void setPan(float newPan) { pan = juce::jlimit(-1.0f, 1.0f, newPan); }
    float getPan() const { return pan; }

    void setReverbSend(float newSend) { reverbSend = juce::jlimit(0.0f, 1.0f, newSend); }
    float getReverbSend() const { return reverbSend; }

    // EQ parameters
    void setLowCutFreq(float freq) { lowCutFreq = juce::jlimit(20.0f, 500.0f, freq); filtersNeedUpdate = true; }
    float getLowCutFreq() const { return lowCutFreq; }

    void setHighCutFreq(float freq) { highCutFreq = juce::jlimit(1000.0f, 20000.0f, freq); filtersNeedUpdate = true; }
    float getHighCutFreq() const { return highCutFreq; }

    void setMidEqFreq(float freq) { midEqFreq = juce::jlimit(200.0f, 8000.0f, freq); filtersNeedUpdate = true; }
    float getMidEqFreq() const { return midEqFreq; }

    void setMidEqGain(float gain) { midEqGain = juce::jlimit(-12.0f, 12.0f, gain); filtersNeedUpdate = true; }
    float getMidEqGain() const { return midEqGain; }

    // Set sample rate for filter calculations
    void setSampleRate(double sr) { sampleRate = sr; filtersNeedUpdate = true; }

    // Input channel configuration
    void setInputMode(InputMode mode) { inputMode = mode; }
    InputMode getInputMode() const { return inputMode; }

    void setMonoInputChannel(int channel) { inputChannelLeft = channel; }
    void setStereoInputChannels(int leftChannel, int rightChannel)
    {
        inputChannelLeft = leftChannel;
        inputChannelRight = rightChannel;
    }

    int getInputChannelLeft() const { return inputChannelLeft; }
    int getInputChannelRight() const { return inputChannelRight; }

    // Track info
    int getTrackNumber() const { return trackNumber; }
    void setTrackName(const juce::String& name) { trackName = name; }
    juce::String getTrackName() const { return trackName; }

    // Level metering
    float getPeakLevel() const { return peakLevel.load(); }
    void updateInputMeter(float inputPeak);  // Update meter from live input without writing to buffer

    // Buffer management
    void clearBuffer();
    int getBufferLength() const { return recordedAudio.getNumSamples(); }
    void ensureBufferSize(int numSamples);
    void loadAudioData(const juce::AudioBuffer<float>& sourceBuffer);

    // File I/O
    bool saveToFile(const juce::File& file, double sampleRate);
    bool loadFromFile(const juce::File& file);
    bool hasRecordedAudio() const;

    // File-based recording
    void startRecordingToFile(const juce::File& file, double sampleRate);
    void stopRecordingToFile();
    // Punch-aware stop: flushes the file writer but does NOT reload from file,
    // preserving the full memory buffer (pre-punch + punch content).
    void stopRecordingToFileForPunch();
    bool isRecordingToFile() const { return activeWriter != nullptr; }
    juce::File getCurrentRecordingFile() const { return currentRecordingFile; }

private:
    int trackNumber;
    juce::String trackName;

    // Audio buffer for recorded/playback audio
    juce::AudioBuffer<float> recordedAudio;
    int playbackPosition = 0;

    // Track state
    std::atomic<bool> recordArmed { false };
    std::atomic<bool> muted { false };
    std::atomic<bool> solo { false };
    std::atomic<float> volume { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<float> reverbSend { 0.0f };

    // EQ parameters
    std::atomic<float> lowCutFreq { 20.0f };      // 20Hz - 500Hz (20Hz = effectively off)
    std::atomic<float> highCutFreq { 20000.0f };  // 1kHz - 20kHz (20kHz = effectively off)
    std::atomic<float> midEqFreq { 1000.0f };     // 200Hz - 8kHz
    std::atomic<float> midEqGain { 0.0f };        // -12dB to +12dB (0 = no boost/cut)

    // Filter state (only used in audio thread)
    juce::IIRFilter lowCutFilterL, lowCutFilterR;
    juce::IIRFilter highCutFilterL, highCutFilterR;
    juce::IIRFilter midEqFilterL, midEqFilterR;
    std::atomic<bool> filtersNeedUpdate { true };
    double sampleRate = 44100.0;

    void updateFilters();

    // Input channel configuration
    InputMode inputMode = InputMode::Mono;
    int inputChannelLeft = 0;   // For mono: the channel; for stereo: left channel
    int inputChannelRight = 1;  // For stereo: right channel

    // Level metering (peak level for VU meters)
    std::atomic<float> peakLevel { 0.0f };

    // Cached flag for whether we have audio (thread-safe)
    std::atomic<bool> hasAudio { false };
    int cachedBufferLength = 0; // Length of valid audio data

    // File-based recording
    std::unique_ptr<juce::AudioFormatWriter> activeWriter;
    juce::File currentRecordingFile;
    juce::WavAudioFormat wavFormat;
    juce::CriticalSection writerLock;

    // Write buffer for efficient file I/O
    juce::AudioBuffer<float> writeBuffer;
    int writeBufferPosition = 0;
    static constexpr int WRITE_BUFFER_SIZE = 2048; // Buffer 2048 samples before writing

    void flushWriteBuffer();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Track)
};
