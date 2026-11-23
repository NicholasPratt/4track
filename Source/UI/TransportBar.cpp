#include "TransportBar.h"

TransportBar::TransportBar(AudioEngine& e, Project& p, TimelineView& v) : engine(e), project(p), view(v)
{
    projectName.setFont(Theme::font(14.0f, true));
    projectName.setColour(juce::Label::textColourId, Theme::text);
    addAndMakeVisible(projectName);

    toStart.setTooltip("Go to start (Return)");
    play.setTooltip("Play (Space)");
    stop.setTooltip("Stop (Space)");
    record.setTooltip("Record (R)");
    toStart.onClick = [this] { if (onToStart) onToStart(); };
    play.onClick    = [this] { if (onPlay) onPlay(); };
    stop.onClick    = [this] { if (onStop) onStop(); };
    record.onClick  = [this] { if (onRecord) onRecord(); };
    for (auto* b : { &toStart, &play, &stop, &record })
        addAndMakeVisible(b);

    bpm.setSliderStyle(juce::Slider::LinearBarVertical);
    bpm.setRange(30.0, 300.0, 0.5);
    bpm.setTextBoxIsEditable(true);
    bpm.textFromValueFunction = [](double value) { return juce::String(value, value == std::floor(value) ? 0 : 1) + " BPM"; };
    bpm.valueFromTextFunction = [](const juce::String& text) { return text.retainCharacters("0123456789.").getDoubleValue(); };
    bpm.setColour(juce::Slider::trackColourId, Theme::control);
    bpm.setColour(juce::Slider::textBoxTextColourId, Theme::text);
    bpm.setTooltip("Tempo: drag up/down or double-click to type");
    bpm.setScrollWheelEnabled(false);
    bpm.onValueChange = [this]
    {
        engine.bpm = bpm.getValue();
        project.markDirty();
        if (onGridChanged) onGridChanged();
    };
    addAndMakeVisible(bpm);

    for (int n = 2; n <= 7; ++n)
        timeSig.addItem(juce::String(n) + "/4", n);
    timeSig.setTooltip("Beats per bar");
    timeSig.onChange = [this]
    {
        engine.beatsPerBar = timeSig.getSelectedId();
        project.markDirty();
        if (onGridChanged) onGridChanged();
    };
    addAndMakeVisible(timeSig);

    click.onClick   = [this] { engine.metronomeOn = click.getToggleState(); project.markDirty(); };
    countIn.onClick = [this] { engine.countInOn = countIn.getToggleState(); project.markDirty(); };
    punch.onClick   = [this]
    {
        engine.punchOn = punch.getToggleState();
        project.markDirty();
        if (onGridChanged) onGridChanged();
    };
    snap.onClick    = [this] { view.snapOn = snap.getToggleState(); };
    mixer.onClick   = [this] { if (onToggleMixer) onToggleMixer(); };
    zoomIn.onClick  = [this] { if (onZoomIn) onZoomIn(); };
    zoomOut.onClick = [this] { if (onZoomOut) onZoomOut(); };
    zoomIn.setTooltip("Zoom in (=)");
    zoomOut.setTooltip("Zoom out (-)");

    for (auto* b : { (juce::Component*) &click, (juce::Component*) &countIn, (juce::Component*) &punch,
                     (juce::Component*) &snap, (juce::Component*) &mixer, (juce::Component*) &zoomIn,
                     (juce::Component*) &zoomOut })
        addAndMakeVisible(b);

    addAndMakeVisible(masterMeter);
    refresh();
}

void TransportBar::refresh()
{
    bpm.setValue(engine.bpm.load(), juce::dontSendNotification);
    timeSig.setSelectedId(engine.beatsPerBar.load(), juce::dontSendNotification);
    click.setToggleState(engine.metronomeOn.load(), juce::dontSendNotification);
    countIn.setToggleState(engine.countInOn.load(), juce::dontSendNotification);
    punch.setToggleState(engine.punchOn.load(), juce::dontSendNotification);
    snap.setToggleState(view.snapOn, juce::dontSendNotification);
}

void TransportBar::tick()
{
    const auto state = engine.getState();
    play.setToggleState(state != AudioEngine::State::Stopped, juce::dontSendNotification);
    record.setToggleState(state == AudioEngine::State::Recording, juce::dontSendNotification);
    punch.setToggleState(engine.punchOn.load(), juce::dontSendNotification);
    click.setToggleState(engine.metronomeOn.load(), juce::dontSendNotification);
    bpm.setEnabled(state == AudioEngine::State::Stopped);
    timeSig.setEnabled(state == AudioEngine::State::Stopped);

    const auto pos = engine.getPosition();
    const auto grid = Grid::make(engine, view);
    auto newBars = grid.barsBeats(pos);
    auto newTime = formatTime(pos, engine.getSampleRate());
    if (newBars != barsText || newTime != timeText)
    {
        barsText = newBars;
        timeText = newTime;
        repaint(displayArea);
    }

    const auto name = project.getName() + (project.isDirty() ? "  \xe2\x80\xa2" : "");
    if (projectName.getText() != name)
        projectName.setText(name, juce::dontSendNotification);
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);
    g.setColour(Theme::bg);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float) getWidth());

    // Position display
    const bool recording = engine.getState() == AudioEngine::State::Recording;
    g.setColour(Theme::bg);
    g.fillRoundedRectangle(displayArea.toFloat(), 5.0f);
    if (recording)
    {
        g.setColour(Theme::record.withAlpha(0.6f));
        g.drawRoundedRectangle(displayArea.toFloat().reduced(0.5f), 5.0f, 1.0f);
    }

    auto r = displayArea.reduced(10, 3);
    g.setColour(recording ? Theme::record.brighter(0.4f) : Theme::text);
    g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 19.0f, juce::Font::bold));
    g.drawText(barsText, r.removeFromTop(r.getHeight() * 3 / 5), juce::Justification::centredLeft, false);
    g.setColour(Theme::textDim);
    g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.drawText(timeText, r, juce::Justification::centredLeft, false);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced(10, 9);

    projectName.setBounds(r.removeFromLeft(170));
    r.removeFromLeft(6);

    for (auto* b : { &toStart, &play, &stop, &record })
    {
        b->setBounds(r.removeFromLeft(38));
        r.removeFromLeft(4);
    }
    r.removeFromLeft(8);

    displayArea = r.removeFromLeft(150);
    r.removeFromLeft(10);

    bpm.setBounds(r.removeFromLeft(84).reduced(0, 3));
    r.removeFromLeft(4);
    timeSig.setBounds(r.removeFromLeft(60).reduced(0, 3));
    r.removeFromLeft(12);

    for (auto* b : { (juce::Button*) &click, (juce::Button*) &countIn, (juce::Button*) &punch, (juce::Button*) &snap })
    {
        b->setBounds(r.removeFromLeft(b == &countIn ? 72 : 56).reduced(0, 3));
        r.removeFromLeft(4);
    }

    masterMeter.setBounds(r.removeFromRight(110).withSizeKeepingCentre(110, 10));
    r.removeFromRight(10);
    mixer.setBounds(r.removeFromRight(60).reduced(0, 3));
    r.removeFromRight(10);
    zoomIn.setBounds(r.removeFromRight(28).reduced(0, 3));
    r.removeFromRight(2);
    zoomOut.setBounds(r.removeFromRight(28).reduced(0, 3));
}
