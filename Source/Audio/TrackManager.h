#pragma once

#include "Track.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>

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

private:
    static constexpr int NUM_TRACKS = 4;
    std::array<std::unique_ptr<Track>, NUM_TRACKS> tracks;

    int currentPosition = 0;
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackManager)
};
