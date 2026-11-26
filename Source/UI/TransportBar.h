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
    void tick(bool canRecall);
    /** Re-reads tempo/toggles after a project load. */
    void refresh();
    void showRangeMode();
    void showTuner();

    std::function<void()> onRecall, onPlay, onStop, onRecord, onToStart, onRangeMode;
    std::function<void()> onZoomIn, onZoomOut, onToggleMixer, onGridChanged;
    std::function<void()> onOpenDrumPatterns;

    void setMasterPeaks(float l, float r) { masterMeter.setPeaks(l, r); }
    void setMixerVisible(bool visible) { mixer.setToggleState(visible, juce::dontSendNotification); }
    void setDrumPatternsAvailable(bool available) { drums.setEnabled(available); }

private:
    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    juce::Label projectName;
    juce::TextButton recall { "Keep what I just played" };
    IconButton toStart { "Go to start", IconButton::Icon::ToStart },
               play    { "Play", IconButton::Icon::Play, Theme::monitor },
               stop    { "Stop", IconButton::Icon::Stop },
               record  { "Record", IconButton::Icon::Record, Theme::record };

    juce::Rectangle<int> displayArea;
    juce::String barsText, timeText;

    /** Shows the tempo and time signature; opens the TempoPopup. */
    juce::TextButton tempo;
    juce::Component::SafePointer<juce::CallOutBox> tempoPopup;
    void showTempoPopup();
    void showTempo();

    IconButton click { "Metronome", IconButton::Icon::Metronome },
               countIn { "Recording count-in", IconButton::Icon::CountIn },
               tuner { "Tuner", IconButton::Icon::Tuner };
    std::unique_ptr<juce::DocumentWindow> tunerWindow;
    ToggleChip punch   { "Punch", Theme::record, "Off / Punch / Loop (P). Punch records only inside the range; Loop repeats it, "
                                                 "and recording in a loop works like a looper. Drag on the lower strip of the ruler to set the range." },
               snap    { "Snap", Theme::accent, "Snap edits to the grid (hold Cmd while dragging to bypass)" },
               mixer   { "Mixer", Theme::accent, "Show the mixer (X)" };
    juce::TextButton drums { "Drums" }, zoomOut { "-" }, zoomIn { "+" };

    LevelMeter masterMeter { false };
};
