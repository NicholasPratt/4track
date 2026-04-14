#include "AudioSettingsDialog.h"

AudioSettingsDialog::AudioSettingsDialog(juce::AudioDeviceManager& deviceManager)
    : audioDeviceManager(deviceManager)
{
    // Create the built-in JUCE audio device selector component
    audioSetupComp = std::make_unique<juce::AudioDeviceSelectorComponent>(
        audioDeviceManager,
        0,     // minInputChannels
        256,   // maxInputChannels
        2,     // minOutputChannels
        256,   // maxOutputChannels
        false, // showMidiInputOptions
        false, // showMidiOutputOptions
        true,  // showChannelsAsStereoPairs
        false  // hideAdvancedOptionsWithButton
    );
    addAndMakeVisible(*audioSetupComp);

    // Close button
    closeButton.setButtonText("Close");
    closeButton.addListener(this);
    addAndMakeVisible(closeButton);

    setSize(500, 400);
}

AudioSettingsDialog::~AudioSettingsDialog()
{
    closeButton.removeListener(this);
}

void AudioSettingsDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 2);
}

void AudioSettingsDialog::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Close button at bottom
    auto buttonArea = bounds.removeFromBottom(30);
    closeButton.setBounds(buttonArea.removeFromRight(80));

    bounds.removeFromBottom(10); // spacing

    // Audio setup component takes the rest
    audioSetupComp->setBounds(bounds);
}

void AudioSettingsDialog::buttonClicked(juce::Button* button)
{
    if (button == &closeButton)
    {
        // Close the dialog window
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }
}
