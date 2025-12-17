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
}

void TrackStrip::timerCallback()
{
    updateMeters();
}

void TrackStrip::updateMeters()
{
    if (track)
    {
        float peakLevel = track->getPeakLevel();
        vuMeter.setLevel(peakLevel);
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

    // VU meter
    vuMeter.setBounds(bounds.removeFromTop(100).reduced(5));

    // Volume slider on left
    auto volumeArea = bounds.removeFromLeft(40);
    volumeSlider.setBounds(volumeArea.removeFromTop(150).reduced(5));

    // Pan knob below volume
    panKnob.setBounds(volumeArea.removeFromTop(80).reduced(2));

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
}

void TrackStrip::buttonClicked(juce::Button* button)
{
    if (!track)
        return;

    if (button == &armButton)
    {
        track->setRecordArmed(armButton.getToggleState());
        if (armButton.getToggleState())
        {
            track->clearBuffer();
        }
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
