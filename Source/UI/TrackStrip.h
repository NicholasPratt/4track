#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "VUMeter.h"
#include "../Audio/Track.h"

class TrackStrip : public juce::Component,
                   private juce::Slider::Listener,
                   private juce::Button::Listener,
                   private juce::ComboBox::Listener,
                   private juce::Timer
{
public:
    TrackStrip(Track* track);
    ~TrackStrip() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Update meters from audio thread (call this regularly)
    void updateMeters();

    // Update input channel options based on available inputs
    void updateInputChannelOptions(int numAvailableInputs);

    // Reset UI controls to defaults (call after newProject)
    void resetUI();

private:
    void sliderValueChanged(juce::Slider* slider) override;
    void buttonClicked(juce::Button* button) override;
    void comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged) override;
    void timerCallback() override;

    Track* track;

    // UI Components
    juce::Label trackLabel;
    juce::Label inputLabel;
    juce::ComboBox inputSelector;
    VUMeter vuMeter;
    juce::Slider volumeSlider;
    juce::Slider panKnob;
    juce::Slider sendKnob;
    juce::Label sendLabel;

    // EQ controls
    juce::Label eqLabel;
    juce::Slider lowCutKnob;
    juce::Slider highCutKnob;
    juce::Slider midFreqKnob;
    juce::Slider midGainKnob;
    juce::Label lowCutLabel;
    juce::Label highCutLabel;
    juce::Label midFreqLabel;
    juce::Label midGainLabel;

    juce::TextButton armButton;
    juce::TextButton muteButton;
    juce::TextButton soloButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackStrip)
};
