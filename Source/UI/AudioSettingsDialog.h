#pragma once

#include "../Audio/AudioEngine.h"
#include <juce_audio_utils/juce_audio_utils.h>

/** The audio device chooser, plus which input channels are recorded as stereo pairs. */
class AudioSettingsDialog : public juce::Component,
                            private juce::ChangeListener
{
public:
    explicit AudioSettingsDialog(AudioEngine&);
    ~AudioSettingsDialog() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuildPairs();

    AudioEngine& engine;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    juce::Label pairsTitle;
    juce::OwnedArray<juce::ToggleButton> pairs;
    juce::TextButton closeButton { "Close" };

    static constexpr int kSelectorHeight = 360, kPairColumns = 4, kPairRowHeight = 24;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSettingsDialog)
};
