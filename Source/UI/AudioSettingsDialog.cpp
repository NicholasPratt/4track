#include "AudioSettingsDialog.h"
#include "Theme.h"

AudioSettingsDialog::AudioSettingsDialog(AudioEngine& e) : engine(e)
{
    selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        engine.getDeviceManager(),
        0, 256,    // input channels
        2, 256,    // output channels
        false, false,
        false,     // list channels one by one: pairs are chosen below
        false);
    addAndMakeVisible(*selector);

    pairsTitle.setText("Stereo input pairs", juce::dontSendNotification);
    pairsTitle.setFont(Theme::font(13.0f, true));
    pairsTitle.setColour(juce::Label::textColourId, Theme::text);
    addAndMakeVisible(pairsTitle);

    closeButton.onClick = [this]
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    addAndMakeVisible(closeButton);

    engine.addChangeListener(this);
    rebuildPairs();
}

AudioSettingsDialog::~AudioSettingsDialog()
{
    engine.removeChangeListener(this);
}

void AudioSettingsDialog::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuildPairs();
}

void AudioSettingsDialog::rebuildPairs()
{
    pairs.clear();
    for (auto& in : engine.getInputs())
    {
        const int left = in.channel - in.channel % 2;
        if (left != in.channel || !engine.canPairInputs(left))
            continue;
        auto* toggle = pairs.add(new juce::ToggleButton("In " + juce::String(left + 1) + "/" + juce::String(left + 2)));
        toggle->setToggleState(engine.isInputPaired(left), juce::dontSendNotification);
        toggle->setTooltip("Record inputs " + juce::String(left + 1) + " and " + juce::String(left + 2) + " together as one stereo input");
        toggle->onClick = [this, left, toggle]
        {
            // Rebuilding the inputs calls rebuildPairs (deleting this button), so do it afterwards.
            const bool stereo = toggle->getToggleState();
            juce::MessageManager::callAsync([safe = SafePointer<AudioSettingsDialog>(this), left, stereo]
            {
                if (safe != nullptr)
                    safe->engine.setInputPaired(left, stereo);
            });
        };
        addAndMakeVisible(toggle);
    }

    const int rows = juce::jmax(1, (pairs.size() + kPairColumns - 1) / kPairColumns);
    setSize(560, 10 + kSelectorHeight + 10 + 24 + rows * kPairRowHeight + 20 + 30 + 10);
    resized();
}

void AudioSettingsDialog::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);
    if (pairs.isEmpty())
    {
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(12.0f));
        g.drawText("Turn on two or more input channels above to pair them.",
                   pairsTitle.getBounds().translated(0, 24).withWidth(getWidth() - 20), juce::Justification::centredLeft);
    }
}

void AudioSettingsDialog::resized()
{
    auto r = getLocalBounds().reduced(10);
    closeButton.setBounds(r.removeFromBottom(30).removeFromRight(80));
    r.removeFromBottom(20);

    selector->setBounds(r.removeFromTop(kSelectorHeight));
    r.removeFromTop(10);
    pairsTitle.setBounds(r.removeFromTop(24));

    const int w = r.getWidth() / kPairColumns;
    for (int i = 0; i < pairs.size(); ++i)
        pairs[i]->setBounds(r.getX() + (i % kPairColumns) * w, r.getY() + (i / kPairColumns) * kPairRowHeight, w, kPairRowHeight);
}
