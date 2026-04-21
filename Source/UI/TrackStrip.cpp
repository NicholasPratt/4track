#include "TrackStrip.h"

TrackStrip::TrackStrip(Track* t)
    : track(t)
{
    // Track label
    trackLabel.setText(track ? track->getTrackName() : "Track", juce::dontSendNotification);
    trackLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    trackLabel.setJustificationType(juce::Justification::centred);
    trackLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(trackLabel);

    // Input label
    inputLabel.setText("Input:", juce::dontSendNotification);
    inputLabel.setFont(juce::Font(11.0f));
    inputLabel.setJustificationType(juce::Justification::centredLeft);
    inputLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(inputLabel);

    // Input selector
    inputSelector.setColour(juce::ComboBox::backgroundColourId, juce::Colours::black);
    inputSelector.setColour(juce::ComboBox::textColourId, juce::Colours::white);
    inputSelector.setColour(juce::ComboBox::outlineColourId, juce::Colours::white);
    inputSelector.setColour(juce::ComboBox::arrowColourId, juce::Colours::white);
    inputSelector.addListener(this);
    addAndMakeVisible(inputSelector);

    // Initialize with default channel options (will be updated when device is known)
    updateInputChannelOptions(8); // Default to 8 inputs

    // VU Meter
    addAndMakeVisible(vuMeter);

    // Volume slider (vertical)
    volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    volumeSlider.setRange(0.0, 1.0, 0.01);
    volumeSlider.setValue(track ? track->getVolume() : 0.8);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setColour(juce::Slider::backgroundColourId, juce::Colours::black);
    volumeSlider.setColour(juce::Slider::trackColourId, juce::Colours::white);
    volumeSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    volumeSlider.addListener(this);
    addAndMakeVisible(volumeSlider);

    // Pan knob (rotary)
    panKnob.setSliderStyle(juce::Slider::Rotary);
    panKnob.setRange(-1.0, 1.0, 0.01);
    panKnob.setValue(track ? track->getPan() : 0.0);
    panKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 15);
    panKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::white);
    panKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    panKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    panKnob.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
    panKnob.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::black);
    panKnob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::white);
    panKnob.addListener(this);
    addAndMakeVisible(panKnob);

    // Reverb send knob
    sendLabel.setText("SEND", juce::dontSendNotification);
    sendLabel.setFont(juce::Font(10.0f));
    sendLabel.setJustificationType(juce::Justification::centred);
    sendLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(sendLabel);

    sendKnob.setSliderStyle(juce::Slider::Rotary);
    sendKnob.setRange(0.0, 1.0, 0.01);
    sendKnob.setValue(track ? track->getReverbSend() : 0.0);
    sendKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sendKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::cyan);
    sendKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    sendKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    sendKnob.addListener(this);
    addAndMakeVisible(sendKnob);

    // EQ Section label
    eqLabel.setText("EQ", juce::dontSendNotification);
    eqLabel.setFont(juce::Font(10.0f, juce::Font::bold));
    eqLabel.setJustificationType(juce::Justification::centred);
    eqLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(eqLabel);

    // Low Cut knob (High-pass filter)
    lowCutLabel.setText("LC", juce::dontSendNotification);
    lowCutLabel.setFont(juce::Font(9.0f));
    lowCutLabel.setJustificationType(juce::Justification::centred);
    lowCutLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(lowCutLabel);

    lowCutKnob.setSliderStyle(juce::Slider::Rotary);
    lowCutKnob.setRange(20.0, 500.0, 1.0);
    lowCutKnob.setSkewFactorFromMidPoint(100.0);  // Logarithmic feel
    lowCutKnob.setValue(track ? track->getLowCutFreq() : 20.0);
    lowCutKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    lowCutKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::orange);
    lowCutKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    lowCutKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    lowCutKnob.addListener(this);
    addAndMakeVisible(lowCutKnob);

    // High Cut knob (Low-pass filter)
    highCutLabel.setText("HC", juce::dontSendNotification);
    highCutLabel.setFont(juce::Font(9.0f));
    highCutLabel.setJustificationType(juce::Justification::centred);
    highCutLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(highCutLabel);

    highCutKnob.setSliderStyle(juce::Slider::Rotary);
    highCutKnob.setRange(1000.0, 20000.0, 10.0);
    highCutKnob.setSkewFactorFromMidPoint(5000.0);  // Logarithmic feel
    highCutKnob.setValue(track ? track->getHighCutFreq() : 20000.0);
    highCutKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    highCutKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::orange);
    highCutKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    highCutKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    highCutKnob.addListener(this);
    addAndMakeVisible(highCutKnob);

    // Mid EQ Frequency knob
    midFreqLabel.setText("FRQ", juce::dontSendNotification);
    midFreqLabel.setFont(juce::Font(9.0f));
    midFreqLabel.setJustificationType(juce::Justification::centred);
    midFreqLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(midFreqLabel);

    midFreqKnob.setSliderStyle(juce::Slider::Rotary);
    midFreqKnob.setRange(200.0, 8000.0, 10.0);
    midFreqKnob.setSkewFactorFromMidPoint(1000.0);  // Logarithmic feel
    midFreqKnob.setValue(track ? track->getMidEqFreq() : 1000.0);
    midFreqKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    midFreqKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::orange);
    midFreqKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    midFreqKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    midFreqKnob.addListener(this);
    addAndMakeVisible(midFreqKnob);

    // Mid EQ Gain knob (boost/cut)
    midGainLabel.setText("GAIN", juce::dontSendNotification);
    midGainLabel.setFont(juce::Font(9.0f));
    midGainLabel.setJustificationType(juce::Justification::centred);
    midGainLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(midGainLabel);

    midGainKnob.setSliderStyle(juce::Slider::Rotary);
    midGainKnob.setRange(-12.0, 12.0, 0.5);
    midGainKnob.setValue(track ? track->getMidEqGain() : 0.0);
    midGainKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    midGainKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::orange);
    midGainKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::white);
    midGainKnob.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    midGainKnob.addListener(this);
    addAndMakeVisible(midGainKnob);

    // Arm button
    armButton.setButtonText("ARM");
    armButton.setClickingTogglesState(true);
    armButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    armButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    armButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
    armButton.addListener(this);
    addAndMakeVisible(armButton);

    // Mute button
    muteButton.setButtonText("M");
    muteButton.setClickingTogglesState(true);
    muteButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    muteButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    muteButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::grey);
    muteButton.addListener(this);
    addAndMakeVisible(muteButton);

    // Solo button
    soloButton.setButtonText("S");
    soloButton.setClickingTogglesState(true);
    soloButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    soloButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    soloButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
    soloButton.addListener(this);
    addAndMakeVisible(soloButton);

    // Start timer for meter updates
    startTimerHz(30);
}

TrackStrip::~TrackStrip()
{
    stopTimer();
    inputSelector.removeListener(this);
}

void TrackStrip::timerCallback()
{
    updateMeters();
}

void TrackStrip::resetUI()
{
    armButton.setToggleState(false, juce::dontSendNotification);
    muteButton.setToggleState(false, juce::dontSendNotification);
    soloButton.setToggleState(false, juce::dontSendNotification);
}

void TrackStrip::updateMeters()
{
    if (track)
    {
        float peakLevel = track->getPeakLevel();
        vuMeter.setLevel(peakLevel);

        // Debug logging (once every ~2 seconds at 30Hz timer)
        static int debugCounter = 0;
        if (peakLevel > 0.01f && ++debugCounter % 60 == 0)
        {
            juce::Logger::writeToLog(track->getTrackName() +
                                     " VU meter: peak=" + juce::String(peakLevel, 3));
        }
    }
}

void TrackStrip::paint(juce::Graphics& g)
{
    // Black background with white border
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 2);
}

void TrackStrip::resized()
{
    auto bounds = getLocalBounds().reduced(5);

    // Track label at top
    trackLabel.setBounds(bounds.removeFromTop(25));

    // Input selector area
    auto inputArea = bounds.removeFromTop(40);
    inputLabel.setBounds(inputArea.removeFromTop(15).reduced(2));
    inputSelector.setBounds(inputArea.reduced(2));

    bounds.removeFromTop(5); // spacing

    // VU meter
    vuMeter.setBounds(bounds.removeFromTop(100).reduced(5));

    // Volume slider on left
    auto volumeArea = bounds.removeFromLeft(40);
    volumeSlider.setBounds(volumeArea.removeFromTop(120).reduced(5));

    // Pan knob below volume
    panKnob.setBounds(volumeArea.removeFromTop(60).reduced(2));

    // Send knob below pan
    auto sendArea = volumeArea.removeFromTop(55);
    sendLabel.setBounds(sendArea.removeFromTop(12).reduced(2));
    sendKnob.setBounds(sendArea.reduced(2));

    // EQ section - use the remaining area next to volume
    auto eqArea = bounds.removeFromBottom(90);
    eqLabel.setBounds(eqArea.removeFromTop(14));

    // Four EQ knobs in a row
    auto eqKnobArea = eqArea;
    auto knobWidth = eqKnobArea.getWidth() / 4;

    auto lcArea = eqKnobArea.removeFromLeft(knobWidth);
    lowCutLabel.setBounds(lcArea.removeFromTop(12));
    lowCutKnob.setBounds(lcArea.reduced(2));

    auto hcArea = eqKnobArea.removeFromLeft(knobWidth);
    highCutLabel.setBounds(hcArea.removeFromTop(12));
    highCutKnob.setBounds(hcArea.reduced(2));

    auto frqArea = eqKnobArea.removeFromLeft(knobWidth);
    midFreqLabel.setBounds(frqArea.removeFromTop(12));
    midFreqKnob.setBounds(frqArea.reduced(2));

    auto gainArea = eqKnobArea.removeFromLeft(knobWidth);
    midGainLabel.setBounds(gainArea.removeFromTop(12));
    midGainKnob.setBounds(gainArea.reduced(2));

    // Buttons at bottom
    auto buttonArea = bounds.removeFromBottom(30);
    auto buttonWidth = buttonArea.getWidth() / 3;

    armButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    muteButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
    soloButton.setBounds(buttonArea.removeFromLeft(buttonWidth).reduced(2));
}

void TrackStrip::sliderValueChanged(juce::Slider* slider)
{
    if (!track)
        return;

    if (slider == &volumeSlider)
    {
        track->setVolume(static_cast<float>(volumeSlider.getValue()));
    }
    else if (slider == &panKnob)
    {
        track->setPan(static_cast<float>(panKnob.getValue()));
    }
    else if (slider == &sendKnob)
    {
        track->setReverbSend(static_cast<float>(sendKnob.getValue()));
    }
    else if (slider == &lowCutKnob)
    {
        track->setLowCutFreq(static_cast<float>(lowCutKnob.getValue()));
    }
    else if (slider == &highCutKnob)
    {
        track->setHighCutFreq(static_cast<float>(highCutKnob.getValue()));
    }
    else if (slider == &midFreqKnob)
    {
        track->setMidEqFreq(static_cast<float>(midFreqKnob.getValue()));
    }
    else if (slider == &midGainKnob)
    {
        track->setMidEqGain(static_cast<float>(midGainKnob.getValue()));
    }
}

void TrackStrip::buttonClicked(juce::Button* button)
{
    if (!track)
        return;

    if (button == &armButton)
    {
        track->setRecordArmed(armButton.getToggleState());
        juce::Logger::writeToLog(track->getTrackName() + ": ARM " +
                                 (armButton.getToggleState() ? "ON" : "OFF"));
    }
    else if (button == &muteButton)
    {
        track->setMuted(muteButton.getToggleState());
    }
    else if (button == &soloButton)
    {
        track->setSolo(soloButton.getToggleState());
    }
}

void TrackStrip::comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged)
{
    if (!track || comboBoxThatHasChanged != &inputSelector)
        return;

    int selectedId = inputSelector.getSelectedId();

    if (selectedId == 2000)
    {
        // Bounce: record from the main stereo mix output
        track->setInputMode(Track::InputMode::BounceFromMix);
    }
    else if (selectedId >= 1000)
    {
        // Stereo pair (ID = 1000 + left_channel)
        int leftChannel = selectedId - 1000;
        int rightChannel = leftChannel + 1;
        track->setInputMode(Track::InputMode::Stereo);
        track->setStereoInputChannels(leftChannel, rightChannel);
    }
    else if (selectedId > 0)
    {
        // Mono channel (ID = channel_index + 1)
        int channel = selectedId - 1;
        track->setInputMode(Track::InputMode::Mono);
        track->setMonoInputChannel(channel);
    }
}

void TrackStrip::updateInputChannelOptions(int numAvailableInputs)
{
    int currentSelection = inputSelector.getSelectedId();

    inputSelector.clear();

    // Add mono options
    for (int ch = 0; ch < numAvailableInputs; ++ch)
    {
        juce::String itemText = "Ch " + juce::String(ch + 1) + " (Mono)";
        inputSelector.addItem(itemText, ch + 1); // ID = channel + 1
    }

    // Add stereo pair options
    for (int ch = 0; ch < numAvailableInputs - 1; ch += 2)
    {
        juce::String itemText = "Ch " + juce::String(ch + 1) + "+" + juce::String(ch + 2) + " (Stereo)";
        inputSelector.addItem(itemText, 1000 + ch); // ID = 1000 + left_channel
    }

    // Bounce option
    inputSelector.addSeparator();
    inputSelector.addItem("Mix Out (Bounce)", 2000);

    // Restore previous selection if still valid, otherwise select first mono channel
    if (currentSelection > 0 && inputSelector.indexOfItemId(currentSelection) >= 0)
        inputSelector.setSelectedId(currentSelection, juce::dontSendNotification);
    else
        inputSelector.setSelectedId(1, juce::dontSendNotification); // Default to Ch 1 (Mono)

    // Always push the current selection into the track model. Using dontSendNotification
    // above suppresses the comboBoxChanged callback, so the track's input mode would
    // otherwise never be applied (especially on startup / device change).
    comboBoxChanged(&inputSelector);
}
