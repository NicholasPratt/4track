#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "TrackStrip.h"
#include "TransportBar.h"
#include <memory>
#include <array>

class MainComponent : public juce::Component,
                     private juce::Button::Listener,
                     private juce::ChangeListener,
                     public juce::KeyListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;

    // KeyListener (global key handler — registered on top-level window)
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;
    using juce::Component::keyPressed; // un-hide the 1-arg Component::keyPressed

private:
    void buttonClicked(juce::Button* button) override;
    void showAudioSettingsDialog();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void updateTrackInputSelectors();

    // Project management
    void newProject();
    void saveProject();
    void loadProject();
    void renderProject();
    void autoSaveTracks();
    juce::File getProjectFolder();
    void createProjectFolderIfNeeded();

    std::unique_ptr<AudioEngine> audioEngine;

    // UI components
    std::array<std::unique_ptr<TrackStrip>, 4> trackStrips;
    std::unique_ptr<TransportBar> transportBar;
    juce::Label titleLabel;
    juce::Label versionLabel;
    juce::TextButton settingsButton;
    juce::TextButton newButton;
    juce::TextButton saveButton;
    juce::TextButton loadButton;
    juce::TextButton renderButton;

    // Project folder
    juce::File currentProjectFolder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
