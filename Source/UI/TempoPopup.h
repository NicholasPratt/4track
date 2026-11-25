#pragma once

#include "Widgets.h"
#include "../Audio/AudioEngine.h"

/**
    The tempo settings, shown in a callout from the transport bar's tempo button:
    the tempo (drag, type, nudge, halve/double), tap tempo (the Tap button or the
    space bar while the popup is open), the time signature, and which MIDI outputs
    send MIDI clock.

    Tempo and time signature can only change while stopped, like before; MIDI clock
    can be switched at any time.
*/
class TempoPopup : public juce::Component, private juce::Timer
{
public:
    explicit TempoPopup(AudioEngine&);

    /** Called after the tempo or time signature changes. */
    std::function<void()> onChanged;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    void setBpm(double newBpm);
    void tap();
    void refresh();
    bool canEditTempo() const { return engine.getState() == AudioEngine::State::Stopped; }

    AudioEngine& engine;

    juce::Slider bpm;
    juce::TextButton minus { "-" }, plus { "+" }, halve { "/2" }, twice { "x2" };
    juce::TextButton tapButton { "Tap" };
    juce::ComboBox timeSig;
    ToggleChip recordOnly { "Record only", Theme::accent, "Play the metronome during recording only" };
    ToggleChip countRecord { "Count in recording", Theme::accent, "One bar before recording from stop" };
    ToggleChip countPlayback { "Count in playback", Theme::accent, "One bar before playback from stop" };
    juce::OwnedArray<ToggleChip> clockOutputs;

    std::vector<double> taps;   // ms timestamps of the current run of taps
    double lastTapTime = 0;

    juce::Rectangle<int> tempoHeading, sigLabel, metronomeHeading, clockHeading, hintArea;
    juce::String hint;
};
