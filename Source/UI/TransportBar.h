#pragma once

#include "Widgets.h"
#include "TimelineView.h"
#include "../Project/Project.h"

/** Top toolbar: transport, position display, tempo, recording options, view controls. */
class TransportBar : public juce::Component
{
public:
    TransportBar(AudioEngine&, Project&, TimelineView&);

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Call from the UI timer. */
    void tick();
    /** Re-reads tempo/toggles after a project load. */
    void refresh();
    void showRangeMode();

    std::function<void()> onPlay, onStop, onRecord, onToStart, onRangeMode;
    std::function<void()> onZoomIn, onZoomOut, onToggleMixer, onGridChanged;

    void setMasterPeaks(float l, float r) { masterMeter.setPeaks(l, r); }
    void setMixerVisible(bool visible) { mixer.setToggleState(visible, juce::dontSendNotification); }

private:
    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    juce::Label projectName;
    IconButton toStart { "Go to start", IconButton::Icon::ToStart },
               play    { "Play", IconButton::Icon::Play, Theme::monitor },
               stop    { "Stop", IconButton::Icon::Stop },
               record  { "Record", IconButton::Icon::Record, Theme::record };

    juce::Rectangle<int> displayArea;
    juce::String barsText, timeText;

    juce::Slider bpm;
    juce::ComboBox timeSig;

    ToggleChip click   { "Click", Theme::accent, "Metronome (C)" },
               countIn { "Count-in", Theme::accent, "One bar of click before recording starts" },
               punch   { "Punch", Theme::record, "Off / Punch / Loop (P). Punch records only inside the range; Loop repeats it, "
                                                 "and recording in a loop works like a looper. Drag on the lower strip of the ruler to set the range." },
               snap    { "Snap", Theme::accent, "Snap edits to the grid (hold Cmd while dragging to bypass)" },
               mixer   { "Mixer", Theme::accent, "Show the mixer (X)" };
    juce::TextButton zoomOut { "-" }, zoomIn { "+" };

    LevelMeter masterMeter { false };
};
