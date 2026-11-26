#include "TransportBar.h"
#include "Shortcuts.h"
#include "TempoPopup.h"
#include "TunerWindow.h"

TransportBar::TransportBar(AudioEngine& e, Project& p, TimelineView& v) : engine(e), project(p), view(v)
{
    projectName.setFont(Theme::font(14.0f, true));
    projectName.setColour(juce::Label::textColourId, Theme::text);
    addAndMakeVisible(projectName);
    addChildComponent(recall);
    recall.onClick = [this] { if (onRecall) onRecall(); };

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

    tempo.setTooltip("Tempo, tap tempo, time signature and MIDI clock");
    tempo.setColour(juce::TextButton::buttonOnColourId, Theme::panelLight.brighter(0.2f));
    tempo.onClick = [this] { showTempoPopup(); };
    addAndMakeVisible(tempo);

    click.setClickingTogglesState(true);
    countIn.setClickingTogglesState(true);
    countIn.setTooltip("Recording count-in. Open Tempo for playback count-in.");
    tuner.setTooltip("Tune using armed audio inputs (" + Shortcuts::keysFor(Shortcuts::Context::Global, Shortcuts::cmdShowTuner) + ")");
    tuner.onClick = [this]
    {
        if (tunerWindow != nullptr && tunerWindow->isVisible())
            tunerWindow.reset();
        else
            showTuner();
    };
    click.onClick   = [this] { engine.metronomeOn = click.getToggleState(); project.markDirty(); };
    countIn.onClick = [this] { engine.countInOn = countIn.getToggleState(); project.markDirty(); };
    punch.setClickingTogglesState(false);   // it steps through off, punch and loop
    punch.onClick   = [this] { if (onRangeMode) onRangeMode(); };
    snap.onClick    = [this] { view.snapOn = snap.getToggleState(); };
    mixer.onClick   = [this] { if (onToggleMixer) onToggleMixer(); };
    drums.onClick   = [this] { if (onOpenDrumPatterns) onOpenDrumPatterns(); };
    drums.setTooltip("Open Patterns for the selected Drum Sampler track");
    drums.setEnabled(false);
    zoomIn.onClick  = [this] { if (onZoomIn) onZoomIn(); };
    zoomOut.onClick = [this] { if (onZoomOut) onZoomOut(); };
    zoomIn.setTooltip("Zoom in (=)");
    zoomOut.setTooltip("Zoom out (-)");

    for (auto* b : { (juce::Component*) &click, (juce::Component*) &countIn, (juce::Component*) &tuner, (juce::Component*) &punch,
                     (juce::Component*) &snap, (juce::Component*) &mixer, (juce::Component*) &drums, (juce::Component*) &zoomIn,
                     (juce::Component*) &zoomOut })
        addAndMakeVisible(b);

    addAndMakeVisible(masterMeter);
    refresh();
}

void TransportBar::refresh()
{
    showTempo();
    click.setToggleState(engine.metronomeOn.load(), juce::dontSendNotification);
    countIn.setToggleState(engine.countInOn.load(), juce::dontSendNotification);
    click.setTooltip(engine.metronomeRecordOnly.load() ? "Metronome, record only (C)" : "Metronome (C)");
    showRangeMode();
    snap.setToggleState(view.snapOn, juce::dontSendNotification);
}

void TransportBar::showTempo()
{
    const double value = engine.bpm.load();
    const auto text = juce::String(value, value == std::floor(value) ? 0 : 1) + " BPM   "
                    + juce::String(engine.beatsPerBar.load()) + "/4";
    if (tempo.getButtonText() != text)
        tempo.setButtonText(text);
    tempo.setToggleState(tempoPopup != nullptr, juce::dontSendNotification);
}

void TransportBar::showTempoPopup()
{
    if (tempoPopup != nullptr)
    {
        tempoPopup->dismiss();
        return;
    }
    auto content = std::make_unique<TempoPopup>(engine);
    content->onChanged = [this]
    {
        project.markDirty();
        showTempo();
        if (onGridChanged) onGridChanged();
    };
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(content), tempo.getScreenBounds(), nullptr);
    box.setDismissalMouseClicksAreAlwaysConsumed(true);   // a click on the tempo button just closes it
    tempoPopup = &box;
    showTempo();
}

void TransportBar::showRangeMode()
{
    const auto mode = engine.getRangeMode();
    const auto text = mode == AudioEngine::RangeMode::Loop ? "Loop" : "Punch";
    if (punch.getButtonText() != text)
        punch.setButtonText(text);
    punch.setColour(juce::TextButton::buttonOnColourId, mode == AudioEngine::RangeMode::Loop ? Theme::monitor : Theme::record);
    punch.setToggleState(mode != AudioEngine::RangeMode::Off, juce::dontSendNotification);
}

void TransportBar::tick(bool canRecall)
{
    if (recall.isVisible() != canRecall)
    {
        recall.setVisible(canRecall);
        resized();
    }
    tuner.setToggleState(tunerWindow != nullptr && tunerWindow->isVisible(), juce::dontSendNotification);
    const auto state = engine.getState();
    play.setToggleState(state != AudioEngine::State::Stopped, juce::dontSendNotification);
    // The looper waiting to start or finish blinks the record button.
    const auto looper = engine.getLoopRecord();
    const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
    if (looper == AudioEngine::LoopRecord::Waiting || looper == AudioEngine::LoopRecord::Finishing)
        record.setToggleState(blink, juce::dontSendNotification);
    else
        record.setToggleState(state == AudioEngine::State::Recording, juce::dontSendNotification);
    showRangeMode();
    click.setToggleState(engine.metronomeOn.load(), juce::dontSendNotification);
    showTempo();

    const auto pos = engine.getPosition();
    const auto grid = Grid::make(engine, view);
    auto newBars = grid.barsBeats(pos);
    auto newTime = formatTime(pos, engine.getSampleRate());
    if (looper == AudioEngine::LoopRecord::Waiting || looper == AudioEngine::LoopRecord::Finishing)
    {
        const auto boundary = grid.barsBeats(engine.punchOut.load());
        newTime = (looper == AudioEngine::LoopRecord::Waiting ? "Rec starts at " : "Rec ends at ") + boundary;
    }
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

void TransportBar::showTuner()
{
    if (tunerWindow != nullptr && tunerWindow->isVisible())
        tunerWindow->toFront(true);
    else
        tunerWindow = openTunerWindow(engine, tuner.getScreenBounds());
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
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 19.0f, juce::Font::bold)));
    g.drawText(barsText, r.removeFromTop(r.getHeight() * 3 / 5), juce::Justification::centredLeft, false);
    g.setColour(Theme::textDim);
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    g.drawText(timeText, r, juce::Justification::centredLeft, false);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced(10, 9);

    auto title = r.removeFromLeft(170);
    if (recall.isVisible())
        recall.setBounds(title.removeFromBottom(19));
    projectName.setBounds(title);
    r.removeFromLeft(6);

    for (auto* b : { &toStart, &play, &stop, &record })
    {
        b->setBounds(r.removeFromLeft(38));
        r.removeFromLeft(4);
    }
    r.removeFromLeft(8);

    displayArea = r.removeFromLeft(138);
    r.removeFromLeft(10);

    tempo.setBounds(r.removeFromLeft(110).reduced(0, 3));
    r.removeFromLeft(12);

    for (auto* b : { (juce::Button*) &click, (juce::Button*) &countIn, (juce::Button*) &tuner, (juce::Button*) &punch, (juce::Button*) &snap })
    {
        b->setBounds(r.removeFromLeft(b == &punch || b == &snap ? 56 : 38).reduced(0, 3));
        r.removeFromLeft(4);
    }

    masterMeter.setBounds(r.removeFromRight(96).withSizeKeepingCentre(96, 10));
    r.removeFromRight(10);
    mixer.setBounds(r.removeFromRight(60).reduced(0, 3));
    r.removeFromRight(10);
    drums.setBounds(r.removeFromRight(64).reduced(0, 3));
    r.removeFromRight(8);
    zoomIn.setBounds(r.removeFromRight(28).reduced(0, 3));
    r.removeFromRight(2);
    zoomOut.setBounds(r.removeFromRight(28).reduced(0, 3));
}
