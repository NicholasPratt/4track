#pragma once

#include "TimelineView.h"
#include "../Project/Project.h"
#include <optional>
#include <set>

/**
    The clip area: one horizontal lane per track.

    Mouse:
      click a clip            select it (shift-click adds to the selection)
      drag a clip             move it, in time and onto other tracks (snaps to the grid)
      drag a clip's edge      trim it
      drag a top corner       adjust the fade-in or fade-out
      click empty space       move the playhead there
      right-click a clip      split / delete / reset fades
      wheel / trackpad        scroll;  cmd + wheel or pinch to zoom
      hold cmd while dragging to turn snapping off for that drag
      drop audio files        import them at the drop point

    During a drag the edit is applied to a preview copy of the arrangement. It's
    committed as one undo step on mouse-up.
*/
class TrackLanes : public juce::Component,
                   public juce::FileDragAndDropTarget
{
public:
    TrackLanes(AudioEngine&, Project&, TimelineView&);

    static constexpr int kTrackHeight = 84;
    int scrollY = 0;
    int getContentHeight() const { return kTrackHeight * kNumTracks; }
    int trackAtY(float y) const { return juce::jlimit(0, kNumTracks - 1, (int) std::floor((y + (float) scrollY) / kTrackHeight)); }

    const std::set<juce::int64>& getSelection() const { return selection; }
    void selectAll();
    void clearSelection() { selection.clear(); repaint(); }
    void pruneSelection();

    std::function<void()> onViewChanged;    // scrolled or zoomed
    std::function<void(int)> onTrackClicked;

    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    enum class Zone { None, Body, TrimStart, TrimEnd, FadeIn, FadeOut };
    struct Hit { juce::int64 clipId = 0; int track = -1; Zone zone = Zone::None; };

    Hit findHit(juce::Point<float> p) const;
    juce::Rectangle<float> clipRect(const Clip& c, int track) const;
    juce::int64 snapped(juce::int64 s, const juce::ModifierKeys& mods) const;
    std::function<bool(Arrangement&)> editForDrag(const juce::MouseEvent& e) const;
    void showClipMenu(juce::int64 clipId);

    void paintGrid(juce::Graphics&, float top, float bottom);
    void paintClip(juce::Graphics&, const Clip&, int track, bool selected, bool hovered);
    void paintLiveTakes(juce::Graphics&);
    template <typename MinMaxFn>
    void paintWaveform(juce::Graphics&, juce::Rectangle<float> area, juce::int64 clipStart, MinMaxFn&& minMaxForRange,
                       juce::Colour colour, const Clip* envelope);

    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    std::set<juce::int64> selection;
    Hit hover;

    // Drag state
    Hit dragHit;
    bool dragActive = false;
    juce::int64 dragDownSample = 0;
    int dragDownTrack = 0;
    std::optional<Arrangement> preview;
};
