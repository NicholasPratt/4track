#include "TransportBar.h"

TransportBar::TransportBar(TransportController* t, double sr)
    : transport(t), sampleRate(sr)
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

    // RTZ (Return to Zero) button
    rtzButton.setButtonText("RTZ");
    rtzButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
    rtzButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    rtzButton.addListener(this);
    addAndMakeVisible(rtzButton);

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

    // Update counter at 10Hz
    startTimerHz(10);
}

TransportBar::~TransportBar()
{
    stopTimer();
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

    // Update button states based on transport state
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
    counterDisplay.setBounds(bounds.removeFromLeft(180).reduced(5));

    bounds.removeFromLeft(20); // spacing

    // Transport buttons
    auto buttonWidth = 70;
    auto buttonHeight = 40;
    auto spacing = 8;

    rwdButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
    bounds.removeFromLeft(spacing);

    playButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
    bounds.removeFromLeft(spacing);

    stopButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
    bounds.removeFromLeft(spacing);

    recordButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
    bounds.removeFromLeft(spacing);

    ffdButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
    bounds.removeFromLeft(spacing);

    rtzButton.setBounds(bounds.removeFromLeft(buttonWidth).withHeight(buttonHeight));
}

void TransportBar::buttonClicked(juce::Button* button)
{
    if (!transport)
        return;

    if (button == &playButton)
    {
        transport->play();
    }
    else if (button == &stopButton)
    {
        // Stop now pauses (doesn't reset position)
        transport->pause();
    }
    else if (button == &recordButton)
    {
        transport->record();
    }
    else if (button == &rtzButton)
    {
        // Return to zero - stop and reset position
        transport->stop();
    }
    else if (button == &rwdButton)
    {
        // Rewind 5 seconds
        transport->rewind(5.0, sampleRate);
    }
    else if (button == &ffdButton)
    {
        // Fast forward 5 seconds
        transport->fastForward(5.0, sampleRate);
    }
}
