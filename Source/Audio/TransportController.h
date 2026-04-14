#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

class TransportController
{
public:
    enum class State
    {
        STOPPED,
        PLAYING,
        RECORDING
    };

    TransportController();
    ~TransportController();

    // Transport control
    void play();
    void stop();
    void pause();  // Stop without resetting position
    void record();
    void togglePlayback();
    void fastForward(double seconds, double sampleRate);
    void rewind(double seconds, double sampleRate);

    // State queries
    State getState() const { return currentState.load(); }
    bool isPlaying() const { return currentState.load() == State::PLAYING; }
    bool isRecording() const { return currentState.load() == State::RECORDING; }
    bool isStopped() const { return currentState.load() == State::STOPPED; }

    // Position management
    void setPosition(juce::int64 newPosition) { position = newPosition; }
    juce::int64 getPosition() const { return position.load(); }
    void resetPosition() { position = 0; }

    // Advance position (called from audio thread)
    void advance(int numSamples) { position += numSamples; }

    // Time conversion helpers (requires sample rate)
    double getPositionInSeconds(double sampleRate) const;
    void setPositionInSeconds(double seconds, double sampleRate);

    // Markers (stored in samples, -1 = not set)
    void setMarkerOne(juce::int64 pos) { markerOne = pos; }
    void setMarkerTwo(juce::int64 pos) { markerTwo = pos; }
    juce::int64 getMarkerOne() const { return markerOne.load(); }
    juce::int64 getMarkerTwo() const { return markerTwo.load(); }
    bool isMarkerOneSet() const { return markerOne.load() >= 0; }
    bool isMarkerTwoSet() const { return markerTwo.load() >= 0; }
    void clearMarkerOne() { markerOne = -1; }
    void clearMarkerTwo() { markerTwo = -1; }

    // Punch mode: auto-record between M1 and M2 during playback
    void setPunchMode(bool enabled) { punchModeEnabled = enabled; }
    bool isPunchModeEnabled() const { return punchModeEnabled.load(); }

    // Callbacks for state changes
    void onStateChanged(std::function<void(State)> callback) { stateChangedCallback = callback; }

private:
    void setState(State newState);

    std::atomic<State> currentState { State::STOPPED };
    std::atomic<juce::int64> position { 0 };

    std::atomic<juce::int64> markerOne { -1 };
    std::atomic<juce::int64> markerTwo { -1 };
    std::atomic<bool> punchModeEnabled { false };

    std::function<void(State)> stateChangedCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportController)
};
