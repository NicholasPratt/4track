#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "TrackStrip.h"
#include "TransportBar.h"
#include <memory>
#include <array>

class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    std::unique_ptr<AudioEngine> audioEngine;

    // UI components
    std::array<std::unique_ptr<TrackStrip>, 4> trackStrips;
    std::unique_ptr<TransportBar> transportBar;
    juce::Label titleLabel;
    juce::Label versionLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
