#pragma once

#include "TimelineView.h"
#include "../Project/Project.h"
#include <map>
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
      drag on empty space     draw a rectangle: selects the clips it touches (shift adds to the
                              selection), then offers join / normalise / transpose and more
      right-click a clip      cut / copy / paste / duplicate / split / delete / fades / gain /
                              join / normalise / transpose
      right-click empty space paste the copied clips there
      wheel / trackpad        scroll;  cmd + wheel or pinch to zoom
      hold cmd while dragging to turn snapping off for that drag
      drop audio files        import them at the drop point
      double-click            on a MIDI clip: edit its notes; on an empty MIDI lane: add a one-bar clip

    A track showing automation (chosen with the A button in its header) draws the curve over
    its clips, and the mouse edits the curve instead: click to add a point, drag a point to
    move it, double-click a point to delete it, right-click to clear or go back to the clips.

    During a drag the edit is applied to a preview copy of the arrangement. It's
    committed as one undo step on mouse-up.
*/
class TrackLanes : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   public juce::DragAndDropTarget
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
    void setSelection(std::set<juce::int64> ids) { selection = std::move(ids); repaint(); }
    void pruneSelection();

    std::function<void()> onViewChanged;    // scrolled or zoomed
    std::function<void(int)> onTrackClicked;
    std::function<void(juce::int64)> onEditMidiClip;   // open the note editor for this clip

    // The clipboard lives with the Edit menu; the right-click menus use it through these.
    std::function<void()> onCut, onCopy, onDuplicate;
    std::function<void(int track, juce::int64 position)> onPaste;
    std::function<bool()> canPaste;

    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scale) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    enum class Zone { None, Body, TrimStart, TrimEnd, FadeIn, FadeOut };
    struct Hit { juce::int64 clipId = 0; int track = -1; Zone zone = Zone::None; };

    Hit findHit(juce::Point<float> p) const;
    juce::Rectangle<float> clipRect(const Clip& c, int track) const;
    juce::int64 snapped(juce::int64 s, const juce::ModifierKeys& mods) const;
    std::function<bool(Arrangement&)> editForDrag(const juce::MouseEvent& e) const;
    juce::Rectangle<float> takeControl(const Clip&, int track) const;
    void showClipMenu(juce::int64 clipId, int track, juce::int64 position);
    void showEmptyMenu(int track, juce::int64 position);
    /** Join, normalise and transpose, for the selection. */
    void addSelectionActions(juce::PopupMenu&) const;
    /** Runs a menu result from addSelectionActions; false if it wasn't one of those. */
    bool performSelectionAction(int result);
    void updateMarquee(const juce::MouseEvent&);

    void paintGrid(juce::Graphics&, float top, float bottom);
    void paintClip(juce::Graphics&, const Clip&, int track, bool selected, bool hovered);
    void paintLiveTakes(juce::Graphics&);
    void paintNotes(juce::Graphics&, const Clip&, juce::Rectangle<float> area, juce::Colour);

    // Automation
    te::AutomatableParameter* automationFor(int track) const;
    juce::Rectangle<float> automationArea(int track) const;
    float yForValue(const te::AutomatableParameter&, float value, int track) const;
    float valueAtY(const te::AutomatableParameter&, float y, int track) const;
    int automationPointAt(const te::AutomatableParameter&, int track, juce::Point<float>) const;
    void paintAutomation(juce::Graphics&, int track, te::AutomatableParameter&);
    void automationMouseDown(const juce::MouseEvent&, int track, te::AutomatableParameter&);
    void showAutomationMenu(int track, te::AutomatableParameter&);
    struct AutomationDrag { int track = -1, index = -1; te::AutomatableParameter::Ptr param; } autoDrag;
    te::SmartThumbnail& thumbnailFor(const juce::File&);

    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    std::set<juce::int64> selection;
    Hit hover;

    struct MidiDrop
    {
        int track;
        te::TimeRange range;
        juce::String name;
        int patternSlot = -1;
        te::BeatPosition patternStart;
    };
    std::optional<MidiDrop> midiDropAt(const SourceDetails&);
    void selectDroppedClip(te::MidiClip*, int track);
    std::optional<MidiDrop> midiDropPreview;

    // Drag state
    Hit dragHit;
    bool dragActive = false;
    juce::int64 dragDownSample = 0;
    int dragDownTrack = 0;
    std::optional<Arrangement> preview;

    // Rectangle selection, while dragging on empty space
    std::optional<juce::Rectangle<float>> marquee;
    std::set<juce::int64> marqueeBase;   // the selection it adds to

    // Waveforms, built and cached by Tracktion. Keyed by file path.
    std::map<juce::String, std::unique_ptr<te::SmartThumbnail>> thumbnails;
    std::map<juce::String, te::RecordingThumbnailManager::Thumbnail::Ptr> recordingThumbs;
};
