#pragma once

#include "Widgets.h"
#include "../Project/Project.h"

/**
    Note editor for one MIDI clip, shown in the bottom panel.

    Mouse:
      click empty space        add a note (then drag to set its length)
      drag a note              move it in time and pitch
      drag a note's right end  change its length
      double-click a note      delete it (or select it and press Delete)
      wheel                    scroll the keyboard;  hold cmd while dragging to turn snapping off

    Notes snap to sixteenths. Each gesture is one undo step in the Edit's undo history.
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

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    static constexpr int kKeyboardWidth = 44, kHeaderHeight = 26, kRowHeight = 9;

private:
    te::MidiClip* clip() const { return project.getMidiClip(clipId); }
    juce::UndoManager& undo() { return project.getEdit().getUndoManager(); }

    juce::Rectangle<int> gridArea() const;
    juce::Range<double> visibleBeats() const;   // content beats from the clip's start to its end
    float xForBeat(double beat) const;
    double beatAtX(float x) const;
    int noteAtY(float y) const;
    float yForNote(int note) const;
    double snap(double beat, const juce::ModifierKeys&) const;
    te::MidiNote* noteAt(juce::Point<float>, bool& onRightEdge) const;
    void centreOnNotes();

    AudioEngine& engine;
    Project& project;
    juce::int64 clipId = 0;
    int topNote = 84;   // highest visible note

    juce::TextButton closeButton { "Close" };

    // Drag state
    enum class Drag { None, Move, Resize };
    Drag drag = Drag::None;
    juce::ValueTree dragNote;   // the note's state: survives the MidiList rebuilding its objects
    double dragStartBeat = 0, dragLength = 1, dragDownBeat = 0;
    int dragPitch = 60, dragDownNote = 60;
    juce::ValueTree selected;
    double lastLength = 1.0;
};
