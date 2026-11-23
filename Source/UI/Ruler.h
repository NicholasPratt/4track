#pragma once

#include "TimelineView.h"
#include "../Project/Project.h"

/**
    Timeline ruler.
      Top part:    bar numbers. Click or drag to move the playhead.
      Bottom strip: punch range. Drag on empty space to draw a new range (this turns
                    punch on). Drag an edge to adjust it, drag inside to move it, and
                    double-click to toggle punch on/off.
*/
class Ruler : public juce::Component
{
public:
    Ruler(AudioEngine&, Project&, TimelineView&);

    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;

    std::function<void()> onViewChanged;
    std::function<void()> onPunchChanged;

private:
    enum class Drag { None, Locate, PunchNew, PunchIn, PunchOut, PunchMove };

    bool inPunchStrip(float y) const { return y >= (float) getHeight() - kPunchStripHeight; }
    Drag punchZoneAt(float x) const;
    juce::int64 snapped(juce::int64 s, const juce::ModifierKeys& mods) const;

    static constexpr float kPunchStripHeight = 14.0f;

    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    Drag drag = Drag::None;
    juce::int64 dragAnchor = 0, dragOrigIn = 0, dragOrigOut = 0;
};
