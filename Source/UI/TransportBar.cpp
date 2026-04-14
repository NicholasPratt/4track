#include "TransportBar.h"
#include "../Audio/TrackManager.h"

TransportBar::TransportBar(TransportController* t, TrackManager* tm, double sr)
    : transport(t), trackManager(tm), sampleRate(sr)
{
    // Counter display (old-school LED style)
    counterDisplay.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::bold));
    counterDisplay.setJustificationType(juce::Justification::centred);
    counterDisplay.setColour(juce::Label::backgroundColourId, juce::Colours::black);
    counterDisplay.setColour(juce::Label::textColourId, juce::Colours::white);
    counterDisplay.setColour(juce::Label::outlineColourId, juce::Colours::white);
    counterDisplay.setText("00:00:00", juce::dontSendNotification);
    addAndMakeVisible(counterDisplay);

    // Play button
    playButton.setButtonText("PLAY");
    playButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    playButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    playButton.addListener(this);
    addAndMakeVisible(playButton);

    // Stop button
    stopButton.setButtonText("STOP");
    stopButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    stopButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    stopButton.addListener(this);
    addAndMakeVisible(stopButton);

    // Record button
    recordButton.setButtonText("REC");
    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    recordButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    recordButton.addListener(this);
    addAndMakeVisible(recordButton);
    juce::Logger::writeToLog("TransportBar: REC button created and listener added");

    // Rewind button
    rwdButton.setButtonText("RWD");
    rwdButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    rwdButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    rwdButton.addListener(this);
    addAndMakeVisible(rwdButton);

    // Fast Forward button
    ffdButton.setButtonText("FFD");
    ffdButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    ffdButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    ffdButton.addListener(this);
    addAndMakeVisible(ffdButton);

    // Reverb section label
    reverbLabel.setText("REVERB", juce::dontSendNotification);
    reverbLabel.setFont(juce::Font(12.0f, juce::Font::bold));
    reverbLabel.setJustificationType(juce::Justification::centred);
    reverbLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(reverbLabel);

    // Room size knob
    roomLabel.setText("ROOM", juce::dontSendNotification);
    roomLabel.setFont(juce::Font(9.0f));
    roomLabel.setJustificationType(juce::Justification::centred);
    roomLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(roomLabel);

    roomSizeKnob.setSliderStyle(juce::Slider::Rotary);
    roomSizeKnob.setRange(0.0, 1.0, 0.01);
    roomSizeKnob.setValue(trackManager ? trackManager->getReverbRoomSize() : 0.5);
    roomSizeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    roomSizeKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::cyan);
    roomSizeKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    roomSizeKnob.addListener(this);
    addAndMakeVisible(roomSizeKnob);

    // Damping knob
    dampLabel.setText("DAMP", juce::dontSendNotification);
    dampLabel.setFont(juce::Font(9.0f));
    dampLabel.setJustificationType(juce::Justification::centred);
    dampLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(dampLabel);

    dampingKnob.setSliderStyle(juce::Slider::Rotary);
    dampingKnob.setRange(0.0, 1.0, 0.01);
    dampingKnob.setValue(trackManager ? trackManager->getReverbDamping() : 0.5);
    dampingKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    dampingKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::cyan);
    dampingKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    dampingKnob.addListener(this);
    addAndMakeVisible(dampingKnob);

    // Mix/Wet level knob
    mixLabel.setText("MIX", juce::dontSendNotification);
    mixLabel.setFont(juce::Font(9.0f));
    mixLabel.setJustificationType(juce::Justification::centred);
    mixLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(mixLabel);

    reverbMixKnob.setSliderStyle(juce::Slider::Rotary);
    reverbMixKnob.setRange(0.0, 1.0, 0.01);
    reverbMixKnob.setValue(trackManager ? trackManager->getReverbWetLevel() : 0.33);
    reverbMixKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    reverbMixKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::cyan);
    reverbMixKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    reverbMixKnob.addListener(this);
    addAndMakeVisible(reverbMixKnob);

    // Marker 1 button
    m1Button.setButtonText("M1");
    m1Button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff004488));
    m1Button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    m1Button.addListener(this);
    m1Button.addMouseListener(this, false); // catch right-click for clear
    addAndMakeVisible(m1Button);

    // Marker 2 button
    m2Button.setButtonText("M2");
    m2Button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff004488));
    m2Button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    m2Button.addListener(this);
    m2Button.addMouseListener(this, false); // catch right-click for clear
    addAndMakeVisible(m2Button);

    // Punch mode toggle button
    punchButton.setButtonText("PUNCH");
    punchButton.setClickingTogglesState(true);
    punchButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff442200));
    punchButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    punchButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    punchButton.addListener(this);
    addAndMakeVisible(punchButton);

    // Master output level label
    masterLabel.setText("OUT", juce::dontSendNotification);
    masterLabel.setFont(juce::Font(10.0f, juce::Font::bold));
    masterLabel.setJustificationType(juce::Justification::centred);
    masterLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(masterLabel);

    addAndMakeVisible(masterVULeft);
    addAndMakeVisible(masterVURight);

    // Update counter at 10Hz
    startTimerHz(10);
}

TransportBar::~TransportBar()
{
    stopTimer();
}

// Helper to format a sample position as MM:SS
static juce::String formatMarkerTime(juce::int64 pos, double sr)
{
    if (pos < 0 || sr <= 0.0)
        return {};
    double s = static_cast<double>(pos) / sr;
    int m = static_cast<int>(s) / 60;
    int sec = static_cast<int>(s) % 60;
    return juce::String::formatted("%d:%02d", m, sec);
}

void TransportBar::setMarker(int markerNumber)
{
    if (!transport)
        return;

    juce::int64 currentPos = transport->getPosition();

    if (markerNumber == 1)
    {
        if (!transport->isMarkerOneSet())
        {
            transport->setMarkerOne(currentPos);
            punchInFired = false;
        }
        else
        {
            // Jump to marker
            transport->setPosition(transport->getMarkerOne());
            punchInFired = false;
            punchOutFired = false;
        }
    }
    else if (markerNumber == 2)
    {
        if (!transport->isMarkerTwoSet())
        {
            transport->setMarkerTwo(currentPos);
            punchOutFired = false;
        }
        else
        {
            // Jump to marker
            transport->setPosition(transport->getMarkerTwo());
            punchInFired = false;
            punchOutFired = false;
        }
    }
}

void TransportBar::togglePunchMode()
{
    if (!transport)
        return;
    bool newState = !transport->isPunchModeEnabled();
    transport->setPunchMode(newState);
    punchButton.setToggleState(newState, juce::dontSendNotification);
    punchInFired = false;
    punchOutFired = false;
}

void TransportBar::timerCallback()
{
    if (!transport)
        return;

    // Update counter display
    double seconds = transport->getPositionInSeconds(sampleRate);
    int minutes = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int frames = static_cast<int>((seconds - static_cast<int>(seconds)) * 100);

    juce::String timeStr = juce::String::formatted("%02d:%02d:%02d", minutes, secs, frames);
    counterDisplay.setText(timeStr, juce::dontSendNotification);

    // Update transport button states
    auto state = transport->getState();
    if (state == TransportController::State::RECORDING)
    {
        recordButton.setButtonText("REC *");
        playButton.setButtonText("PLAY");
    }
    else if (state == TransportController::State::PLAYING)
    {
        recordButton.setButtonText("REC");
        playButton.setButtonText("PLAY >");
    }
    else
    {
        recordButton.setButtonText("REC");
        playButton.setButtonText("PLAY");
        // Reset punch fire flags when transport is stopped
        punchInFired = false;
        punchOutFired = false;
    }

    // Update marker button labels
    if (transport->isMarkerOneSet())
        m1Button.setButtonText("M1 " + formatMarkerTime(transport->getMarkerOne(), sampleRate));
    else
        m1Button.setButtonText("M1");

    if (transport->isMarkerTwoSet())
        m2Button.setButtonText("M2 " + formatMarkerTime(transport->getMarkerTwo(), sampleRate));
    else
        m2Button.setButtonText("M2");

    // Punch in/out logic
    if (transport->isPunchModeEnabled() &&
        transport->isMarkerOneSet() &&
        transport->isMarkerTwoSet())
    {
        juce::int64 pos = transport->getPosition();
        juce::int64 m1  = transport->getMarkerOne();
        juce::int64 m2  = transport->getMarkerTwo();

        if (state == TransportController::State::PLAYING && !punchInFired && pos >= m1 && pos < m2)
        {
            punchInFired = true;
            punchOutFired = false;
            transport->record(); // triggers startRecordingToFiles via stateChangedCallback
        }
        else if (state == TransportController::State::RECORDING && !punchOutFired && pos >= m2)
        {
            punchOutFired = true;
            transport->play(); // triggers stopRecordingToFilesForPunch via stateChangedCallback
        }
    }

    // Update master output VU meters
    if (trackManager)
    {
        masterVULeft.setLevel(trackManager->getMasterPeakLeft());
        masterVURight.setLevel(trackManager->getMasterPeakRight());
    }
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 2);
}

void TransportBar::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Counter display on left
    counterDisplay.setBounds(bounds.removeFromLeft(160).reduced(5));

    bounds.removeFromLeft(10); // spacing

    // Transport buttons (slightly narrower to make room for markers)
    const int btnW = 58;
    const int btnH = 40;
    const int gap  = 5;

    rwdButton.setBounds(bounds.removeFromLeft(btnW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    playButton.setBounds(bounds.removeFromLeft(btnW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    stopButton.setBounds(bounds.removeFromLeft(btnW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    recordButton.setBounds(bounds.removeFromLeft(btnW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    ffdButton.setBounds(bounds.removeFromLeft(btnW).withHeight(btnH));

    bounds.removeFromLeft(12); // gap before markers

    // Marker buttons
    const int markerW = 62;
    m1Button.setBounds(bounds.removeFromLeft(markerW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    m2Button.setBounds(bounds.removeFromLeft(markerW).withHeight(btnH));
    bounds.removeFromLeft(gap);
    punchButton.setBounds(bounds.removeFromLeft(55).withHeight(btnH));

    bounds.removeFromLeft(15); // gap before reverb section

    // Reverb section
    reverbLabel.setBounds(bounds.removeFromLeft(50).withHeight(15));

    const int knobSize    = 42;
    const int knobSpacing = 4;

    auto roomArea = bounds.removeFromLeft(knobSize);
    roomLabel.setBounds(roomArea.removeFromTop(12));
    roomSizeKnob.setBounds(roomArea.withHeight(knobSize - 12));

    bounds.removeFromLeft(knobSpacing);

    auto dampArea = bounds.removeFromLeft(knobSize);
    dampLabel.setBounds(dampArea.removeFromTop(12));
    dampingKnob.setBounds(dampArea.withHeight(knobSize - 12));

    bounds.removeFromLeft(knobSpacing);

    auto mixArea = bounds.removeFromLeft(knobSize);
    mixLabel.setBounds(mixArea.removeFromTop(12));
    reverbMixKnob.setBounds(mixArea.withHeight(knobSize - 12));

    bounds.removeFromLeft(12); // gap before master meters

    // Master output VU meters (L + R side by side)
    auto masterArea = bounds.removeFromLeft(46);
    masterLabel.setBounds(masterArea.removeFromTop(12));
    const int meterWidth = (masterArea.getWidth() - 3) / 2;
    masterVULeft.setBounds(masterArea.removeFromLeft(meterWidth));
    masterArea.removeFromLeft(3);
    masterVURight.setBounds(masterArea.removeFromLeft(meterWidth));
}

void TransportBar::buttonClicked(juce::Button* button)
{
    juce::Logger::writeToLog("TransportBar: Button clicked - checking which one...");

    if (!transport)
        return;

    if (button == &playButton)
    {
        juce::Logger::writeToLog("TransportBar: PLAY button");
        punchInFired = false;
        punchOutFired = false;
        transport->play();
    }
    else if (button == &stopButton)
    {
        // Check for double-click (within configured window)
        juce::int64 currentTime = juce::Time::currentTimeMillis();
        juce::int64 timeSinceLastClick = currentTime - lastStopClickTime;

        if (timeSinceLastClick < stopDoubleClickMs && lastStopClickTime > 0)
        {
            // Double-click detected - return to zero
            transport->stop();
            juce::Logger::writeToLog("Transport: RTZ (double-click STOP)");
            lastStopClickTime = 0;
        }
        else
        {
            // Single click - pause (keep position)
            transport->pause();
            lastStopClickTime = currentTime;
        }
        punchInFired = false;
        punchOutFired = false;
    }
    else if (button == &recordButton)
    {
        juce::Logger::writeToLog("TransportBar: REC button clicked!");
        transport->record();
    }
    else if (button == &rwdButton)
    {
        transport->rewind(5.0, sampleRate);
        punchInFired = false;
        punchOutFired = false;
    }
    else if (button == &ffdButton)
    {
        transport->fastForward(5.0, sampleRate);
        punchInFired = false;
        punchOutFired = false;
    }
    else if (button == &m1Button)
    {
        // Left-click: if M1 not set, set it; if set, jump to it.
        // Right-click: clear M1 (handled via mouseDown on the button below).
        setMarker(1);
    }
    else if (button == &m2Button)
    {
        setMarker(2);
    }
    else if (button == &punchButton)
    {
        transport->setPunchMode(punchButton.getToggleState());
        punchInFired = false;
        punchOutFired = false;
    }
}

void TransportBar::mouseDown(const juce::MouseEvent& e)
{
    if (!transport || !e.mods.isRightButtonDown())
        return;

    if (e.eventComponent == &m1Button)
    {
        transport->clearMarkerOne();
        punchInFired = false;
    }
    else if (e.eventComponent == &m2Button)
    {
        transport->clearMarkerTwo();
        punchOutFired = false;
    }
}

void TransportBar::sliderValueChanged(juce::Slider* slider)
{
    if (!trackManager)
        return;

    if (slider == &roomSizeKnob)
    {
        trackManager->setReverbRoomSize(static_cast<float>(roomSizeKnob.getValue()));
    }
    else if (slider == &dampingKnob)
    {
        trackManager->setReverbDamping(static_cast<float>(dampingKnob.getValue()));
    }
    else if (slider == &reverbMixKnob)
    {
        trackManager->setReverbWetLevel(static_cast<float>(reverbMixKnob.getValue()));
    }
}
