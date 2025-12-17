#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "VUMeter.h"
#include "../Audio/Track.h"

class TrackStrip : public juce::Component,
                   private juce::Slider::Listener,
                   private juce::Button::Listener,
                   private juce::Timer
{
public:
    TrackStrip(Track* track);
    ~TrackStrip() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Update meters from audio thread (call this regularly)
    void updateMeters();

private:
    void sliderValueChanged(juce::Slider* slider) override;
    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;

    Track* track;

    // UI Components
    juce::Label trackLabel;
    VUMeter vuMeter;
    juce::Slider volumeSlider;
    juce::Slider panKnob;
    juce::TextButton armButton;
    juce::TextButton muteButton;
    juce::TextButton soloButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackStrip)
};
