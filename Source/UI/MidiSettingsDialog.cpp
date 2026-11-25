#include "MidiSettingsDialog.h"

MidiSettingsDialog::MidiSettingsDialog(AudioEngine& e) : engine(e)
{
    closeButton.onClick = [this]
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    addAndMakeVisible(closeButton);

    engine.addChangeListener(this);
    rebuild();
    startTimerHz(20);
}

MidiSettingsDialog::~MidiSettingsDialog()
{
    engine.removeChangeListener(this);
}

void MidiSettingsDialog::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();
}

void MidiSettingsDialog::rebuild()
{
    inputs.clear();
    outputs.clear();

    // Changing a device makes Tracktion rescan, which rebuilds this (deleting the button that
    // was clicked), so each change is made after the click has finished.
    auto later = [this](std::function<void(AudioEngine&)> change)
    {
        juce::MessageManager::callAsync([safe = SafePointer<MidiSettingsDialog>(this), change]
        {
            if (safe != nullptr)
            {
                change(safe->engine);
                safe->rebuild();
            }
        });
    };

    const auto ins = engine.getMidiInputs();
    for (int i = 0; i < (int) ins.size(); ++i)
    {
        auto* row = inputs.add(new InputRow());
        row->name = ins[(size_t) i].name;
        row->enabled.setToggleState(ins[(size_t) i].enabled, juce::dontSendNotification);
        row->enabled.onClick = [later, i, row] { const bool on = row->enabled.getToggleState(); later([i, on](AudioEngine& en) { en.setMidiInputEnabled(i, on); }); };
        addAndMakeVisible(row->enabled);
    }

    const auto outs = engine.getMidiOutputs();
    for (int i = 0; i < (int) outs.size(); ++i)
    {
        const auto& o = outs[(size_t) i];
        auto* row = outputs.add(new OutputRow());
        row->name = o.name;
        row->enabled.setToggleState(o.enabled, juce::dontSendNotification);
        row->clock.setToggleState(o.sendingClock, juce::dontSendNotification);
        row->mmc.setToggleState(o.sendingMMC, juce::dontSendNotification);
        row->mtc.setToggleState(o.sendingTimecode, juce::dontSendNotification);
        row->enabled.onClick = [later, i, row] { const bool on = row->enabled.getToggleState(); later([i, on](AudioEngine& en) { en.setMidiOutputEnabled(i, on); }); };
        row->clock.onClick   = [later, i, row] { const bool on = row->clock.getToggleState();   later([i, on](AudioEngine& en) { en.setSendingMidiClock(i, on); }); };
        row->mmc.onClick     = [later, i, row] { const bool on = row->mmc.getToggleState();     later([i, on](AudioEngine& en) { en.setSendingMMC(i, on); }); };
        row->mtc.onClick     = [later, i, row] { const bool on = row->mtc.getToggleState();     later([i, on](AudioEngine& en) { en.setSendingTimecode(i, on); }); };
        for (auto* b : { &row->clock, &row->mmc, &row->mtc })
            b->setEnabled(o.enabled);

        auto& d = row->delay;
        d.setSliderStyle(juce::Slider::LinearBar);
        d.setRange(-100.0, 250.0, 1.0);
        d.setValue(o.delayMs, juce::dontSendNotification);
        d.textFromValueFunction = [](double v) { return (v > 0 ? "+" : "") + juce::String((int) v) + " ms"; };
        d.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("-0123456789").getDoubleValue(); };
        d.updateText();
        d.setColour(juce::Slider::trackColourId, Theme::control);
        d.setColour(juce::Slider::textBoxTextColourId, Theme::text);
        d.setTooltip("Delays everything sent to this output (clock and notes) to line it up with the audio. "
                     "Negative sends it earlier. Drag, or double-click to type.");
        d.setChangeNotificationOnlyOnRelease(true);   // each change restarts playback
        d.setEnabled(o.enabled);
        d.onValueChange = [later, i, row] { const int ms = (int) row->delay.getValue(); later([i, ms](AudioEngine& en) { en.setMidiOutputDelayMs(i, ms); }); };
        addAndMakeVisible(d);
        for (auto* c : { (juce::Component*) &row->enabled, (juce::Component*) &row->clock,
                         (juce::Component*) &row->mmc, (juce::Component*) &row->mtc })
            addAndMakeVisible(c);
    }

    const int inputRows = juce::jmax(1, inputs.size()), outputRows = juce::jmax(1, outputs.size());
    setSize(kWidth, 12 + 24 + inputRows * kRow + 18 + 24 + outputRows * kRow + 18 + 34 + 30 + 12);
    resized();
    repaint();
}

void MidiSettingsDialog::timerCallback()
{
    // Flash an input's light for a moment after each message.
    const auto ins = engine.getMidiInputs();
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < inputs.size() && i < (int) ins.size(); ++i)
    {
        const bool lit = ins[(size_t) i].lastActivityMs > 0 && now - ins[(size_t) i].lastActivityMs < 150.0;
        if (lit != inputs[i]->lit)
        {
            inputs[i]->lit = lit;
            repaint(inputs[i]->light);
        }
    }
}

void MidiSettingsDialog::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);

    g.setFont(Theme::font(11.0f, true));
    g.setColour(Theme::textDim);
    g.drawText("MIDI INPUTS", inputsHeading, juce::Justification::centredLeft, false);
    g.drawText("MIDI OUTPUTS", outputsHeading, juce::Justification::centredLeft, false);
    g.drawText("DELAY", delayHeading, juce::Justification::centredLeft, false);

    g.setFont(Theme::font(13.0f));
    for (auto* row : inputs)
    {
        g.setColour(row->lit ? Theme::monitor : Theme::bg);
        g.fillEllipse(row->light.toFloat());
        g.setColour(Theme::outline);
        g.drawEllipse(row->light.toFloat(), 1.0f);
        g.setColour(row->enabled.getToggleState() ? Theme::text : Theme::textDim);
        g.drawText(row->name, row->label, juce::Justification::centredLeft, true);
    }
    for (auto* row : outputs)
    {
        g.setColour(row->enabled.getToggleState() ? Theme::text : Theme::textDim);
        g.drawText(row->name, row->label, juce::Justification::centredLeft, true);
    }

    g.setColour(Theme::textDim);
    g.setFont(Theme::font(12.0f));
    if (inputs.isEmpty())
        g.drawText("No MIDI inputs found. Plug one in and it shows up here.", noInputs, juce::Justification::centredLeft, false);
    if (outputs.isEmpty())
        g.drawText("No MIDI outputs found. (On a Mac, the IAC Driver in Audio MIDI Setup adds one.)", noOutputs,
                   juce::Justification::centredLeft, false);
    g.drawFittedText("Each MIDI track picks its input, channel and output from its instrument button.",
                     footer, juce::Justification::centredLeft, 2);
}

void MidiSettingsDialog::resized()
{
    auto r = getLocalBounds().reduced(14, 12);
    closeButton.setBounds(r.removeFromBottom(30).removeFromRight(80));
    footer = r.removeFromBottom(34);
    r.removeFromBottom(18);

    inputsHeading = r.removeFromTop(24);
    noInputs = r.withHeight(kRow);
    for (auto* row : inputs)
    {
        auto line = r.removeFromTop(kRow).reduced(0, 3);
        row->light = line.removeFromLeft(10).withSizeKeepingCentre(10, 10);
        line.removeFromLeft(10);
        row->enabled.setBounds(line.removeFromRight(48));
        row->label = line.reduced(0, 0).withTrimmedRight(10);
    }
    if (inputs.isEmpty())
        r.removeFromTop(kRow);
    r.removeFromTop(18);

    auto heading = r.removeFromTop(24);
    outputsHeading = heading;
    delayHeading = heading.withLeft(heading.getRight() - 110);
    noOutputs = r.withHeight(kRow);
    for (auto* row : outputs)
    {
        auto line = r.removeFromTop(kRow).reduced(0, 3);
        row->delay.setBounds(line.removeFromRight(110));
        line.removeFromRight(10);
        row->mtc.setBounds(line.removeFromRight(48));
        line.removeFromRight(4);
        row->mmc.setBounds(line.removeFromRight(48));
        line.removeFromRight(4);
        row->clock.setBounds(line.removeFromRight(56));
        line.removeFromRight(4);
        row->enabled.setBounds(line.removeFromRight(48));
        row->label = line.withTrimmedRight(10);
    }
}
