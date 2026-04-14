#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/TransportController.h"
#include "VUMeter.h"

class TrackManager;  // Forward declaration

class TransportBar : public juce::Component,
                     private juce::Button::Listener,
                     private juce::Slider::Listener,
                     private juce::Timer
{
public:
    TransportBar(TransportController* transport, TrackManager* trackManager, double sampleRate);
    ~TransportBar() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Update sample rate (call when audio device changes)
    void setSampleRate(double newSampleRate) { sampleRate = newSampleRate; }

    // Marker controls (called from MainComponent key handler)
    // Sets the marker at the current transport position, or jumps to it if already set.
    void setMarker(int markerNumber);   // 1 = M1, 2 = M2
    void togglePunchMode();

private:
    void buttonClicked(juce::Button* button) override;
    void sliderValueChanged(juce::Slider* slider) override;
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent& e) override;

    TransportController* transport;
    TrackManager* trackManager;
    double sampleRate;

    // UI Components
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton recordButton;
    juce::TextButton rwdButton;  // Rewind
    juce::TextButton ffdButton;  // Fast Forward
    juce::Label counterDisplay;

    // For double-click detection on STOP
    juce::int64 lastStopClickTime = 0;
    const int stopDoubleClickMs = 500;

    // Marker buttons (click = jump; [ / ] keys = set; right-click = clear)
    juce::TextButton m1Button;
    juce::TextButton m2Button;
    juce::TextButton punchButton;

    // Track whether punch was already triggered this pass (prevents re-triggering)
    bool punchInFired = false;
    bool punchOutFired = false;

    // Reverb controls
    juce::Label reverbLabel;
    juce::Label roomLabel;
    juce::Label dampLabel;
    juce::Label mixLabel;
    juce::Slider roomSizeKnob;
    juce::Slider dampingKnob;
    juce::Slider reverbMixKnob;

    // Master output level meters
    juce::Label masterLabel;
    VUMeter masterVULeft;
    VUMeter masterVURight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};
