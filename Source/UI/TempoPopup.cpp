#include "TempoPopup.h"

namespace
{
    constexpr int kWidth = 260, kPad = 12, kRowGap = 6;
    constexpr double kMinBpm = 30.0, kMaxBpm = 300.0;
    constexpr double kTapResetMs = 2000.0;   // a longer gap starts a new run of taps
    constexpr size_t kMaxTaps = 8;           // the tempo is the average over the last few taps
}

TempoPopup::TempoPopup(AudioEngine& e) : engine(e)
{
    setWantsKeyboardFocus(true);

    bpm.setSliderStyle(juce::Slider::LinearBarVertical);
    bpm.setRange(kMinBpm, kMaxBpm, 0.5);
    bpm.setTextBoxIsEditable(true);
    bpm.textFromValueFunction = [](double value) { return juce::String(value, value == std::floor(value) ? 0 : 1) + " BPM"; };
    bpm.valueFromTextFunction = [](const juce::String& text) { return text.retainCharacters("0123456789.").getDoubleValue(); };
    bpm.setColour(juce::Slider::trackColourId, Theme::control);
    bpm.setColour(juce::Slider::textBoxTextColourId, Theme::text);
    bpm.setTooltip("Drag up/down or double-click to type");
    bpm.setScrollWheelEnabled(false);
    bpm.onValueChange = [this] { setBpm(bpm.getValue()); };
    addAndMakeVisible(bpm);

    minus.setTooltip("-1 BPM (Down arrow)");
    plus.setTooltip("+1 BPM (Up arrow)");
    halve.setTooltip("Halve the tempo");
    twice.setTooltip("Double the tempo");
    minus.onClick = [this] { setBpm(std::round(engine.bpm.load()) - 1.0); };
    plus.onClick  = [this] { setBpm(std::round(engine.bpm.load()) + 1.0); };
    halve.onClick = [this] { setBpm(engine.bpm.load() / 2.0); };
    twice.onClick = [this] { setBpm(engine.bpm.load() * 2.0); };

    tapButton.setTooltip("Tap the beat here, or with the space bar while this is open");
    tapButton.onClick = [this] { tap(); };

    for (int n = 2; n <= 7; ++n)
        timeSig.addItem(juce::String(n) + "/4", n);
    timeSig.setTooltip("Beats per bar");
    timeSig.onChange = [this]
    {
        if (!canEditTempo() || timeSig.getSelectedId() == engine.beatsPerBar.load())
            return;
        engine.beatsPerBar = timeSig.getSelectedId();
        if (onChanged) onChanged();
    };

    // Keep the keyboard focus on the popup itself, so the space bar always taps.
    for (auto* c : { (juce::Component*) &minus, (juce::Component*) &plus, (juce::Component*) &halve,
                     (juce::Component*) &twice, (juce::Component*) &tapButton, (juce::Component*) &timeSig })
    {
        c->setWantsKeyboardFocus(false);
        addAndMakeVisible(c);
    }

    recordOnly.onClick = [this]
    {
        engine.metronomeRecordOnly = recordOnly.getToggleState();
        if (onChanged) onChanged();
    };
    countRecord.onClick = [this]
    {
        engine.countInOn = countRecord.getToggleState();
        if (onChanged) onChanged();
    };
    countPlayback.onClick = [this]
    {
        engine.playbackCountInOn = countPlayback.getToggleState();
        if (onChanged) onChanged();
    };
    for (auto* button : { &recordOnly, &countRecord, &countPlayback })
    {
        button->setWantsKeyboardFocus(false);
        addAndMakeVisible(button);
    }

    const auto outputs = engine.getMidiOutputs();
    for (int i = 0; i < (int) outputs.size(); ++i)
    {
        auto* chip = clockOutputs.add(new ToggleChip(outputs[(size_t) i].name, Theme::accent,
                                                     "Send MIDI clock to " + outputs[(size_t) i].name));
        chip->setToggleState(outputs[(size_t) i].sendingClock, juce::dontSendNotification);
        chip->setWantsKeyboardFocus(false);
        chip->onClick = [this, i, chip] { engine.setSendingMidiClock(i, chip->getToggleState()); };
        addAndMakeVisible(chip);
    }

    const int clockRows = juce::jmax(1, clockOutputs.size());
    const int height = kPad + 18 + 40 + kRowGap + 26 + kRowGap + 34 + 18 + 26 + 2 * kPad
                     + 18 + 3 * (24 + 4) + kPad + 18 + clockRows * (24 + 4) + kPad;
    setSize(kWidth, height);

    refresh();
    startTimerHz(20);
}

void TempoPopup::parentHierarchyChanged()
{
    // Take the keyboard once the callout is on screen.
    juce::Component::SafePointer<TempoPopup> self(this);
    juce::MessageManager::callAsync([self]
    {
        if (self != nullptr && self->isShowing())
            self->grabKeyboardFocus();
    });
}

void TempoPopup::setBpm(double newBpm)
{
    if (!canEditTempo())
        return;
    newBpm = juce::jlimit(kMinBpm, kMaxBpm, std::round(newBpm * 2.0) / 2.0);   // the slider's 0.5 steps
    if (std::abs(newBpm - engine.bpm.load()) < 1.0e-6)
        return;
    engine.bpm = newBpm;
    bpm.setValue(newBpm, juce::dontSendNotification);
    if (onChanged) onChanged();
}

void TempoPopup::tap()
{
    if (!canEditTempo())
        return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!taps.empty() && now - taps.back() > kTapResetMs)
        taps.clear();
    taps.push_back(now);
    if (taps.size() > kMaxTaps + 1)
        taps.erase(taps.begin());
    lastTapTime = now;

    if (taps.size() >= 2)
    {
        const double interval = (taps.back() - taps.front()) / (double) (taps.size() - 1);
        setBpm(std::round(60000.0 / interval));
    }
    refresh();
}

bool TempoPopup::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::spaceKey)       { tap(); return true; }
    if (code == juce::KeyPress::upKey)      { plus.triggerClick(); return true; }
    if (code == juce::KeyPress::downKey)    { minus.triggerClick(); return true; }
    return false;   // Escape goes on to the callout, which closes
}

void TempoPopup::timerCallback()
{
    refresh();
}

void TempoPopup::refresh()
{
    // Take the keyboard back after typing a tempo, so the space bar taps again.
    if (isShowing() && !hasKeyboardFocus(true) && juce::Process::isForegroundProcess())
        grabKeyboardFocus();

    const bool editable = canEditTempo();
    for (auto* c : { (juce::Component*) &bpm, (juce::Component*) &minus, (juce::Component*) &plus,
                     (juce::Component*) &halve, (juce::Component*) &twice, (juce::Component*) &tapButton,
                     (juce::Component*) &timeSig })
        c->setEnabled(editable);

    // Follow changes made elsewhere (undo, loading a project), but not mid-drag or mid-typing.
    if (!bpm.isMouseButtonDown() && std::abs(bpm.getValue() - engine.bpm.load()) > 1.0e-6)
        bpm.setValue(engine.bpm.load(), juce::dontSendNotification);
    if (timeSig.getSelectedId() != engine.beatsPerBar.load())
        timeSig.setSelectedId(engine.beatsPerBar.load(), juce::dontSendNotification);

    recordOnly.setToggleState(engine.metronomeRecordOnly.load(), juce::dontSendNotification);
    countRecord.setToggleState(engine.countInOn.load(), juce::dontSendNotification);
    countPlayback.setToggleState(engine.playbackCountInOn.load(), juce::dontSendNotification);

    // Flash the Tap button on each tap.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool flash = now - lastTapTime < 100.0;
    tapButton.setToggleState(flash, juce::dontSendNotification);
    tapButton.setColour(juce::TextButton::buttonOnColourId, Theme::accent);

    juce::String newHint;
    if (!editable)
        newHint = "Stop to change the tempo";
    else if (!taps.empty() && now - taps.back() <= kTapResetMs)
        newHint = taps.size() == 1 ? "Keep tapping..." : juce::String((int) taps.size()) + " taps";
    else
        newHint = "Space bar taps while this is open";
    if (newHint != hint)
    {
        hint = newHint;
        repaint(hintArea);
    }
}

void TempoPopup::paint(juce::Graphics& g)
{
    g.setFont(Theme::font(11.0f, true));
    g.setColour(Theme::textDim);
    g.drawText("TEMPO", tempoHeading, juce::Justification::centredLeft, false);
    g.drawText("METRONOME", metronomeHeading, juce::Justification::centredLeft, false);
    g.drawText("MIDI CLOCK OUT", clockHeading, juce::Justification::centredLeft, false);

    g.setFont(Theme::font(11.0f));
    g.drawText(hint, hintArea, juce::Justification::centred, false);

    g.setFont(Theme::font(12.0f));
    g.setColour(Theme::text);
    g.drawText("Time signature", sigLabel, juce::Justification::centredLeft, false);

    g.setColour(Theme::outline);
    g.drawHorizontalLine(metronomeHeading.getY() - kPad, (float) kPad, (float) (getWidth() - kPad));
    g.drawHorizontalLine(clockHeading.getY() - kPad / 2, (float) kPad, (float) (getWidth() - kPad));

    if (clockOutputs.isEmpty())
    {
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(12.0f));
        g.drawText("No MIDI outputs found", clockHeading.translated(0, 18 + 2).withHeight(24),
                   juce::Justification::centredLeft, false);
    }
}

void TempoPopup::resized()
{
    auto r = getLocalBounds().reduced(kPad);

    tempoHeading = r.removeFromTop(18);

    auto row = r.removeFromTop(40);
    minus.setBounds(row.removeFromLeft(36));
    plus.setBounds(row.removeFromRight(36));
    bpm.setBounds(row.reduced(6, 0));
    r.removeFromTop(kRowGap);

    row = r.removeFromTop(26);
    halve.setBounds(row.removeFromLeft(row.getWidth() / 2 - 3));
    twice.setBounds(row.removeFromRight(row.getWidth() - 6));
    r.removeFromTop(kRowGap);

    tapButton.setBounds(r.removeFromTop(34));
    hintArea = r.removeFromTop(18);

    row = r.removeFromTop(26);
    timeSig.setBounds(row.removeFromRight(70));
    sigLabel = row;

    r.removeFromTop(2 * kPad);
    metronomeHeading = r.removeFromTop(18);
    for (auto* button : { &recordOnly, &countRecord, &countPlayback })
    {
        button->setBounds(r.removeFromTop(24));
        r.removeFromTop(4);
    }
    r.removeFromTop(kPad);
    clockHeading = r.removeFromTop(18);
    for (auto* chip : clockOutputs)
    {
        chip->setBounds(r.removeFromTop(24));
        r.removeFromTop(4);
    }
}
