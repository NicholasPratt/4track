#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/TransportController.h"

class TransportBar : public juce::Component,
                     private juce::Button::Listener,
                     private juce::Timer
{
public:
    TransportBar(TransportController* transport, double sampleRate);
    ~TransportBar() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;

    TransportController* transport;
    double sampleRate;

    // UI Components
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton recordButton;
    juce::TextButton rtzButton;  // Return to Zero
    juce::TextButton rwdButton;  // Rewind
    juce::TextButton ffdButton;  // Fast Forward
    juce::Label counterDisplay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};
