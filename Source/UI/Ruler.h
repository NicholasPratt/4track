#pragma once

#include "TimelineView.h"
#include "Shortcuts.h"
#include "../Project/Project.h"

/**
    Timeline ruler.
      Top part:    bar numbers. Click or drag to move the playhead.
      Bottom strip: the punch / loop range. Drag on empty space to draw a new range (this
                    turns punch on if the range was off). Drag an edge to adjust it, drag
                    inside to move it, and double-click to step through off, punch and loop.
*/
class Ruler : public juce::Component, public juce::SettableTooltipClient, public juce::DragAndDropTarget
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
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    static constexpr int baseHeight = 54, chordHeight = 28, lyricRowHeight = 24;
    int timelineHeight() const { return baseHeight + (songwritingExpanded ? chordHeight + visibleLyricRows * lyricRowHeight : 0); }
    void layoutLyrics(int width, int rowLimit);
    int getLyricRowCount() const { return lyricRowCount; }
    int getVisibleLyricRows() const { return visibleLyricRows; }
    int getFirstLyricRow() const { return firstLyricRow; }
    void setFirstLyricRow(int row);

    bool isSongwritingExpanded() const { return songwritingExpanded; }
    void setSongwritingExpanded(bool expanded) { songwritingExpanded = expanded; }
    bool isSongwritingArea(juce::Point<float> p) const { return songwritingExpanded && p.y >= baseHeight; }
    bool hasSongwritingCursor() const;
    bool hasSelectedSongNote() const { return selectedSongNote().has_value(); }
    bool canPasteSongText() const;
    void performSongShortcut(Shortcuts::Command);
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

    std::function<void()> onViewChanged;
    std::function<void()> onPunchChanged;

private:
    enum class Drag { None, Locate, PunchNew, PunchIn, PunchOut, PunchMove };

    bool inPunchStrip(float y) const { return y >= baseHeight - kSectionHeight - kPunchStripHeight && y < baseHeight - kSectionHeight; }
    Drag punchZoneAt(float x) const;
    juce::int64 snapped(juce::int64 s, const juce::ModifierKeys& mods) const;

    juce::Rectangle<float> songLane(Project::SongEntry) const;
    juce::Rectangle<float> chordBounds(const std::vector<Project::SongNote>&, size_t) const;
    std::optional<Project::SongNote> songNoteAt(juce::Point<float>, Project::SongEntry) const;
    void paintSongNotes(juce::Graphics&, Project::SongEntry);
    void songNoteClick(const juce::MouseEvent&, Project::SongEntry);
    void editSongNote(Project::SongEntry, te::BeatPosition, juce::String, juce::ValueTree entry = {});
    te::BeatPosition songBeatAt(float x) const;
    std::optional<te::BeatPosition> lyricDropAt(const SourceDetails&);
    std::optional<Project::SongNote> selectedSongNote() const;
    struct SongCursor
    {
        Project::SongEntry kind;
        te::BeatPosition beat;
        juce::ValueTree edit, entry;
    };
    std::optional<SongCursor> songCursor;
    struct LyricLine { Project::SongNote note; juce::Range<float> span; int row; };
    std::vector<LyricLine> lyricLines;
    int lyricRowCount = 1, visibleLyricRows = 1, firstLyricRow = 0;

    static constexpr float lyricHandleWidth = 15;
    juce::Rectangle<float> lyricBounds(const LyricLine&) const;
    const LyricLine* lyricAt(juce::Point<float>) const;
    struct LyricDrag
    {
        juce::ValueTree entry;
        float grabOffset;
        std::optional<te::BeatPosition> target;
    };
    std::optional<LyricDrag> draggedLyric;
    std::optional<te::BeatPosition> lyricDropBeat;
    bool songwritingExpanded = false;
    std::optional<Project::SongNote> draggedChord;

    void sectionClick(const juce::MouseEvent&);
    void nameSection(te::TimeRange);
    static constexpr float kSectionHeight = 20;
    static constexpr float kPunchStripHeight = 14.0f;

    AudioEngine& engine;
    Project& project;
    TimelineView& view;

    Drag drag = Drag::None;
    juce::int64 dragAnchor = 0, dragOrigIn = 0, dragOrigOut = 0;
};
