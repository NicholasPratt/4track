#pragma once

#include "Widgets.h"
#include "Shortcuts.h"
#include "../Project/Project.h"

/**
    Note editor for one MIDI clip, shown in the bottom panel.

    Mouse:
      click empty space        add a note (then drag to set its length)
      drag a note              move it in time and pitch
      drag a note's right end  change its length
      double-click a note      delete it (or select it and press Delete)
      right-click a note       select it and open its edit menu
      right-drag               select notes in a rectangle (Shift adds to the selection)
      wheel                    scroll the keyboard; hold Cmd while dragging for free placement

    The grid selector controls snapping and quantize. Each gesture is one undo step in the Edit's undo history.
*/
class PianoRoll : public juce::Component
{
public:
    PianoRoll(AudioEngine&, Project&);

    /** Shows this clip (an Arrangement clip id). */
    void setClip(juce::int64 clipId);
    juce::int64 getClipId() const { return clipId; }

    /** Call after the project changes: closes if the clip has gone. */
    void refresh();

    std::function<void()> onClose;

    bool hasSelectedNotes() const { return !selected.empty(); }
    bool canPasteNotesAtCursor() const { return canPasteAt(pasteBeat()); }
    void selectAllNotes();
    void copyNotes();
    void pasteNotesAtCursor() { pasteNotes(pasteBeat()); }
    void deleteNotes();
    void cutNotes() { copyNotes(); deleteNotes(); }
    void performShortcut(Shortcuts::Command);
    bool canDuplicateNotes() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int kKeyboardWidth = 44, kHeaderHeight = 26, kRowHeight = 9, kExpressionHeight = 76;

private:
    te::MidiClip* clip() const { return project.getMidiClip(clipId); }
    juce::UndoManager& undo() { return project.getEdit().getUndoManager(); }

    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> expressionArea() const;
    int controllerNumber() const { return expression.getSelectedId() - 2; }
    float yForValue(int value) const;
    int valueAtY(float y) const;
    void paintExpression(juce::Graphics&, te::MidiClip&);
    void expressionMouseDown(const juce::MouseEvent&);
    void editExpression(const juce::MouseEvent&);
    te::MidiControllerEvent* controllerAt(juce::Point<float>) const;
    juce::Range<double> visibleBeats() const;   // content beats from the clip's start to its end
    float xForBeat(double beat) const;
    double beatAtX(float x) const;
    int noteAtY(float y) const;
    float yForNote(int note) const;
    double snap(double beat, const juce::ModifierKeys&) const;
    juce::Rectangle<float> noteBounds(const te::MidiNote&) const;
    te::MidiNote* noteAt(juce::Point<float>, bool& onRightEdge) const;
    double gridStep() const;
    double editBeat(float x, const juce::ModifierKeys&) const;
    double pasteBeat() const;
    double latestPasteBeat() const;
    bool canPasteAt(double beat) const;
    juce::Range<double> selectedBeatRange() const;
    std::vector<juce::ValueTree> copySelectedNotes(double origin) const;
    void insertNotes(const std::vector<juce::ValueTree>&, double beat, const juce::String& undoName);
    void nudgeNotes(double beats, int semitones);
    bool isSelected(const juce::ValueTree&) const;
    void updateSelectionControls();
    void updateMarquee(const juce::MouseEvent&);
    void showNoteMenu(const juce::MouseEvent&);
    void pasteNotes(double beat);
    void quantizeNotes();
    void centreOnNotes();

    AudioEngine& engine;
    Project& project;
    juce::int64 clipId = 0;
    int topNote = 84;   // highest visible note

    juce::TextButton closeButton { "Close" }, quantize { "Quantize" };
    juce::ComboBox subdivision, expression;

    // Drag state
    enum class Drag { None, Move, Resize, Select, Velocity, Controller };
    Drag drag = Drag::None;
    juce::ValueTree dragController;
    juce::ValueTree dragNote;   // the note's state: survives the MidiList rebuilding its objects
    double dragStartBeat = 0, dragLength = 1, dragDownBeat = 0;
    int dragDownNote = 60;
    struct DraggedNote { juce::ValueTree state; double start, length; int pitch; };
    std::vector<DraggedNote> movingNotes;
    std::vector<juce::ValueTree> selected, selectionBeforeDrag, clipboard;
    std::optional<juce::Rectangle<float>> marquee;
    std::optional<double> insertionBeat;
    float velocityDragStartY = 0;
    std::vector<std::pair<juce::ValueTree, int>> velocityBeforeDrag;
    double lastLength = 1.0;
};
