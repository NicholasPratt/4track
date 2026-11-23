#pragma once

#include "Widgets.h"
#include "../Project/Project.h"

/** Fills an input selector with the device's inputs: mono channels and the stereo pairs set up in Audio Settings. */
void fillInputSelector(juce::ComboBox& box, const std::vector<AudioEngine::Input>& inputs);
/** Selects the item that matches the track's current input setting. */
void showTrackInput(juce::ComboBox& box, const TrackParams& p);
/** Writes the selected item back to the track. */
void applyInputSelection(const juce::ComboBox& box, TrackParams& p);

/** The controls at the left of each lane. */
class TrackHeader : public juce::Component
{
public:
    TrackHeader(AudioEngine&, Project&, int trackIndex);

    void setInputs(const std::vector<AudioEngine::Input>& inputs);
    void refresh();                      // pull values from the track (they may change elsewhere)
    void setMeter(float left, float right) { meter.setPeaks(left, right); }
    void setSelected(bool shouldBeSelected) { if (selected != shouldBeSelected) { selected = shouldBeSelected; repaint(); } }

    std::function<void()> onChanged;     // something the lanes draw (arm state) changed

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    std::function<void()> onClicked;

private:
    void edited();
    void styleMonitorButton();
    void showTrackMenu();
    void showInstrumentMenu();
    void showAutomationMenu();
    void styleAutomationButton();
    void setMidi(bool);
    void updateKind();

    AudioEngine& engine;
    Project& project;
    const int index;
    TrackParams& params;
    bool selected = false;

    juce::Label name;
    juce::TextButton arm { "R" }, mute { "M" }, solo { "S" }, monitor { "I" }, automation { "A" };
    juce::ComboBox input;
    juce::TextButton instrument;   // MIDI tracks show this instead of the input
    bool shownAsMidi = false;
    juce::String shownInstrument;
    juce::Slider volume, pan;
    LevelMeter meter { false };
};
