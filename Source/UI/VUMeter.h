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

    // Decay rate for the needle (makes it fall back naturally)
    static constexpr float decayRate = 0.95f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VUMeter)
};
