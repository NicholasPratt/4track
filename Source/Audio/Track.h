#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

class Track
{
public:
    Track(int trackNumber);
    ~Track();

    // Audio processing
    void processBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples);
    void addInputSample(float leftSample, float rightSample, int position);

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

    // Track info
    int getTrackNumber() const { return trackNumber; }
    void setTrackName(const juce::String& name) { trackName = name; }
    juce::String getTrackName() const { return trackName; }

    // Level metering
    float getPeakLevel() const { return peakLevel.load(); }

    // Buffer management
    void clearBuffer();
    int getBufferLength() const { return recordedAudio.getNumSamples(); }
    void ensureBufferSize(int numSamples);
    void loadAudioData(const juce::AudioBuffer<float>& sourceBuffer);

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

    // Level metering (peak level for VU meters)
    std::atomic<float> peakLevel { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Track)
};
