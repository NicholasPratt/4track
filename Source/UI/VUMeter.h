#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class VUMeter : public juce::Component,
                private juce::Timer
{
public:
    VUMeter();
    ~VUMeter() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Set the current level (0.0 to 1.0)
    void setLevel(float newLevel);

    // Get current level for external monitoring
    float getLevel() const { return currentLevel; }

private:
    void timerCallback() override;

    float currentLevel = 0.0f;
    float displayLevel = 0.0f;  // Smoothed for visual display
    float peakHold = 0.0f;      // Peak hold value
    int peakHoldCounter = 0;    // Frames to hold peak

    // Decay rate for the meter (makes it fall back naturally)
    static constexpr float decayRate = 0.90f;
    static constexpr int peakHoldFrames = 120; // Hold peak for 2 seconds at 60fps

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VUMeter)
};
