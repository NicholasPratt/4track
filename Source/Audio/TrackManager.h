#pragma once

#include "Track.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>
#include <atomic>

class TrackManager
{
public:
    TrackManager();
    ~TrackManager();

    // Audio processing
    void prepareToPlay(double sampleRate, int samplesPerBlockExpected);
    void processBlock(juce::AudioBuffer<float>& outputBuffer,
                     const juce::AudioBuffer<float>& inputBuffer,
                     bool isRecording);
    void releaseResources();

    // Track access
    Track* getTrack(int index);
    const Track* getTrack(int index) const;
    int getNumTracks() const { return 4; }

    // Transport control
    void setPlaybackPosition(int position);
    int getPlaybackPosition() const { return currentPosition; }
    void resetPlaybackPosition() { currentPosition = 0; }

    // Solo logic - check if any track has solo enabled
    bool anySoloEnabled() const;

    // File-based recording
    void startRecordingToFiles(const juce::File& projectFolder);
    void stopRecordingToFiles();
    void stopRecordingToFilesForPunch(); // Flush writers without reloading from file

    // Reverb controls
    void setReverbRoomSize(float value);
    void setReverbDamping(float value);
    void setReverbWetLevel(float value);

    float getReverbRoomSize() const { return reverbRoomSize.load(); }
    float getReverbDamping() const { return reverbDamping.load(); }
    float getReverbWetLevel() const { return reverbWetLevel.load(); }

    // Master output level metering (updated each processBlock)
    float getMasterPeakLeft() const { return masterPeakLeft.load(); }
    float getMasterPeakRight() const { return masterPeakRight.load(); }

private:
    static constexpr int NUM_TRACKS = 4;
    std::array<std::unique_ptr<Track>, NUM_TRACKS> tracks;

    int currentPosition = 0;
    double currentSampleRate = 44100.0;

    // Reverb
    juce::Reverb reverb;
    juce::AudioBuffer<float> reverbSendBuffer;
    juce::AudioBuffer<float> reverbReturnBuffer;

    std::atomic<float> reverbRoomSize { 0.5f };
    std::atomic<float> reverbDamping { 0.5f };
    std::atomic<float> reverbWetLevel { 0.33f };

    std::atomic<float> masterPeakLeft { 0.0f };
    std::atomic<float> masterPeakRight { 0.0f };

    void updateReverbParameters();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackManager)
};
