#pragma once

#include <tracktion_engine/tracktion_engine.h>
#include <array>
#include <atomic>
#include <cstdint>

/** A short audition sent directly to the device output. */
class ChordPreview final : private juce::AudioIODeviceCallback
{
public:
    ~ChordPreview() override;
    void setEnabled(juce::AudioDeviceManager&, bool);
    bool isEnabled() const { return deviceManager != nullptr; }
    bool play(const juce::String& chord);
    void stop();

    static juce::Array<int> notesFor(const juce::String& chord);

private:
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                          const juce::AudioIODeviceCallbackContext&) override;

    static constexpr int maxNotes = 6;
    static constexpr std::uint64_t stopCommand = 1ULL << 63;
    std::atomic<std::uint64_t> command { 0 };
    juce::AudioDeviceManager* deviceManager = nullptr;

    // Audio callback state.
    struct Voice { double sine = 0, cosine = 1, stepSin = 0, stepCos = 0; };
    std::array<Voice, maxNotes> voices;
    int noteCount = 0, elapsed = 0;
    double rate = 44100;
};
