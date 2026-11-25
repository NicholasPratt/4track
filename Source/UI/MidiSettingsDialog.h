#pragma once

#include "Widgets.h"
#include "../Audio/AudioEngine.h"

/**
    The MIDI devices: inputs (on/off, with a light that flashes on incoming messages) and
    outputs (on/off, MIDI clock, MMC, MIDI timecode, and a delay to line a device up with
    the audio). Rebuilds itself when devices are plugged in or unplugged.

    Which input and channel each MIDI track records from, and which output it plays, are
    chosen from the track's instrument button.
*/
class MidiSettingsDialog : public juce::Component,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    explicit MidiSettingsDialog(AudioEngine&);
    ~MidiSettingsDialog() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();

    struct InputRow
    {
        juce::String name;
        ToggleChip enabled { "On", Theme::monitor, "Use this input" };
        juce::Rectangle<int> light, label;
        bool lit = false;
    };
    struct OutputRow
    {
        juce::String name;
        ToggleChip enabled { "On", Theme::monitor, "Use this output" },
                   clock { "Clock", Theme::accent, "Send MIDI clock: start/stop, song position and tempo" },
                   mmc { "MMC", Theme::accent, "Send MIDI Machine Control: play, stop and locate commands" },
                   mtc { "MTC", Theme::accent, "Send MIDI timecode" };
        juce::Slider delay;
        juce::Rectangle<int> label;
    };

    AudioEngine& engine;
    juce::OwnedArray<InputRow> inputs;
    juce::OwnedArray<OutputRow> outputs;
    juce::TextButton closeButton { "Close" };
    juce::Rectangle<int> inputsHeading, outputsHeading, delayHeading, noInputs, noOutputs, footer;

    static constexpr int kWidth = 640, kRow = 28;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiSettingsDialog)
};
