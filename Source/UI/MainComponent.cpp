#include "MainComponent.h"
#include "../Audio/TrackManager.h"

MainComponent::MainComponent()
{
    // Create audio engine
    audioEngine = std::make_unique<AudioEngine>();

    // Title label
    titleLabel.setText("4-TRACK RECORDER", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(28.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    // Version label
    versionLabel.setText("v" APP_VERSION, juce::dontSendNotification);
    versionLabel.setFont(juce::Font(12.0f));
    versionLabel.setJustificationType(juce::Justification::centredRight);
    versionLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(versionLabel);

    // Create 4 track strips
    auto* trackManager = audioEngine->getTrackManager();
    for (int i = 0; i < 4; ++i)
    {
        trackStrips[i] = std::make_unique<TrackStrip>(trackManager->getTrack(i));
        addAndMakeVisible(*trackStrips[i]);
    }

    // Create transport bar
    transportBar = std::make_unique<TransportBar>(
        audioEngine->getTransport(),
        audioEngine->getSampleRate()
    );
    addAndMakeVisible(*transportBar);

    setSize(1000, 700);
}

MainComponent::~MainComponent()
{
    transportBar.reset();
    for (auto& strip : trackStrips)
        strip.reset();
    audioEngine.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    // Black background
    g.fillAll(juce::Colours::black);

    // White border around everything
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 3);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Title at top
    auto titleArea = bounds.removeFromTop(50);
    titleLabel.setBounds(titleArea.removeFromLeft(titleArea.getWidth() - 100));
    versionLabel.setBounds(titleArea);

    bounds.removeFromTop(10); // spacing

    // Transport bar at top
    transportBar->setBounds(bounds.removeFromTop(70).reduced(5));

    bounds.removeFromTop(10); // spacing

    // 4 track strips side by side
    auto trackArea = bounds;
    auto trackWidth = trackArea.getWidth() / 4;

    for (int i = 0; i < 4; ++i)
    {
        trackStrips[i]->setBounds(trackArea.removeFromLeft(trackWidth).reduced(5));
    }
}
