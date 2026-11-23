#pragma once

#include "Widgets.h"
#include "../Project/Project.h"

/** Fills an input selector: mono channels, stereo pairs, and the master mix (for bouncing). */
void fillInputSelector(juce::ComboBox& box, const juce::StringArray& inputNames);
/** Selects the item that matches the track's current input setting. */
void showTrackInput(juce::ComboBox& box, const TrackParams& p);
/** Writes the selected item back to the track. */
void applyInputSelection(const juce::ComboBox& box, TrackParams& p);

/** The controls at the left of each lane. */
class TrackHeader : public juce::Component
{
public:
    TrackHeader(AudioEngine&, Project&, int trackIndex);

    void setInputNames(const juce::StringArray& names);
    void refresh();                      // pull values from the track (they may change elsewhere)
    void setMeter(float left, float right) { meter.setPeaks(left, right); }
    void setSelected(bool shouldBeSelected) { if (selected != shouldBeSelected) { selected = shouldBeSelected; repaint(); } }

    std::function<void()> onChanged;     // something the lanes draw (arm state) changed

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override { if (onClicked) onClicked(); }
    std::function<void()> onClicked;

private:
    void edited();
    void styleMonitorButton();

    AudioEngine& engine;
    Project& project;
    const int index;
    TrackParams& params;
    bool selected = false;

    juce::Label name;
    juce::TextButton arm { "R" }, mute { "M" }, solo { "S" }, monitor { "I" };
    juce::ComboBox input;
    juce::Slider volume, pan;
    LevelMeter meter { false };
};
