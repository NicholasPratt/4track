#include "PianoRoll.h"

namespace
{
    constexpr double kSnap = 0.25;   // a sixteenth, in beats
    constexpr double kMinLength = 1.0 / 32.0;

    bool isBlackKey(int note)
    {
        const int n = note % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    juce::String noteName(int note) { return juce::MidiMessage::getMidiNoteName(note, true, true, 4); }
}

PianoRoll::PianoRoll(AudioEngine& e, Project& p) : engine(e), project(p)
{
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(closeButton);
    setWantsKeyboardFocus(true);
}

void PianoRoll::setClip(juce::int64 id)
{
    clipId = id;
    selected = {};
    drag = Drag::None;
    centreOnNotes();
    repaint();
}

void PianoRoll::refresh()
{
    if (clipId != 0 && clip() == nullptr)
    {
        // The clip went away (deleted, undone, or replaced by an edit that cut it).
        clipId = 0;
        if (onClose)
            onClose();
        return;
    }
    repaint();
}

void PianoRoll::centreOnNotes()
{
    const int rows = juce::jmax(1, gridArea().getHeight() / kRowHeight);
    int centre = 60;
    if (auto* mc = clip(); mc != nullptr && mc->getSequence().getNumNotes() > 0)
    {
        const auto range = mc->getSequence().getNoteNumberRange();
        centre = (range.getStart() + range.getEnd()) / 2;
    }
    topNote = juce::jlimit(rows - 1, 127, centre + rows / 2);
}

//==============================================================================
juce::Rectangle<int> PianoRoll::gridArea() const
{
    return getLocalBounds().withTrimmedTop(kHeaderHeight).withTrimmedLeft(kKeyboardWidth);
}

juce::Range<double> PianoRoll::visibleBeats() const
{
    auto* mc = clip();
    if (mc == nullptr)
        return { 0.0, 4.0 };
    const auto pos = mc->getPosition();
    const double a = mc->getContentBeatAtTime(pos.getStart()).inBeats();
    const double b = mc->getContentBeatAtTime(pos.getEnd()).inBeats();
    return { a, juce::jmax(a + 0.25, b) };
}

float PianoRoll::xForBeat(double beat) const
{
    const auto area = gridArea();
    const auto beats = visibleBeats();
    return (float) area.getX() + (float) ((beat - beats.getStart()) / beats.getLength()) * (float) area.getWidth();
}

double PianoRoll::beatAtX(float x) const
{
    const auto area = gridArea();
    const auto beats = visibleBeats();
    return beats.getStart() + (double) (x - (float) area.getX()) / (double) juce::jmax(1, area.getWidth()) * beats.getLength();
}

int PianoRoll::noteAtY(float y) const
{
    return juce::jlimit(0, 127, topNote - (int) std::floor((y - (float) gridArea().getY()) / (float) kRowHeight));
}

float PianoRoll::yForNote(int note) const
{
    return (float) gridArea().getY() + (float) ((topNote - note) * kRowHeight);
}

double PianoRoll::snap(double beat, const juce::ModifierKeys& mods) const
{
    return mods.isCommandDown() ? beat : std::round(beat / kSnap) * kSnap;
}

te::MidiNote* PianoRoll::noteAt(juce::Point<float> p, bool& onRightEdge) const
{
    onRightEdge = false;
    auto* mc = clip();
    if (mc == nullptr)
        return nullptr;
    const int pitch = noteAtY(p.y);
    for (auto* n : mc->getSequence().getNotes())
    {
        if (n->getNoteNumber() != pitch)
            continue;
        const float x0 = xForBeat(n->getStartBeat().inBeats());
        const float x1 = xForBeat(n->getEndBeat().inBeats());
        if (p.x >= x0 && p.x <= x1 + 2.0f)
        {
            onRightEdge = p.x >= x1 - juce::jmin(6.0f, (x1 - x0) / 3.0f);
            return n;
        }
    }
    return nullptr;
}

//==============================================================================
void PianoRoll::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
    auto* mc = clip();
    const auto area = gridArea();
    const auto colour = juce::Colour(0xffb084f5);

    // Header
    g.setColour(Theme::panel);
    g.fillRect(getLocalBounds().withHeight(kHeaderHeight));
    g.setColour(Theme::text);
    g.setFont(Theme::font(12.0f, true));
    juce::String title = "Notes";
    int track = -1;
    if (mc != nullptr && project.getArrangement().findClip(clipId, &track) != nullptr)
        title << "  -  " << engine.getTrack(track).name;
    g.drawText(title, getLocalBounds().withHeight(kHeaderHeight).withTrimmedLeft(10), juce::Justification::centredLeft);
    g.setColour(Theme::textDim);
    g.setFont(Theme::font(11.0f));
    g.drawText("Click to add  |  drag to move  |  drag the end to resize  |  double-click to delete",
               getLocalBounds().withHeight(kHeaderHeight).withTrimmedLeft(220).withTrimmedRight(90),
               juce::Justification::centredLeft, true);

    if (mc == nullptr)
        return;

    // Rows and keyboard
    for (int note = topNote; yForNote(note) < (float) area.getBottom() && note >= 0; --note)
    {
        const float y = yForNote(note);
        const bool black = isBlackKey(note);
        g.setColour(black ? Theme::bg.darker(0.25f) : Theme::panel.darker(0.1f));
        g.fillRect((float) area.getX(), y, (float) area.getWidth(), (float) kRowHeight);
        g.setColour(black ? juce::Colour(0xff202226) : juce::Colour(0xffd8d8d8));
        g.fillRect(0.0f, y, (float) kKeyboardWidth - 1.0f, (float) kRowHeight - 0.5f);
        if (note % 12 == 0)
        {
            g.setColour(juce::Colours::black.withAlpha(0.7f));
            g.setFont(Theme::font(8.0f, true));
            g.drawText(noteName(note), juce::Rectangle<float>(2.0f, y, (float) kKeyboardWidth - 4.0f, (float) kRowHeight),
                       juce::Justification::centredRight);
            g.setColour(Theme::outline);
            g.drawHorizontalLine((int) (y + (float) kRowHeight), (float) area.getX(), (float) area.getRight());
        }
    }

    // Beat grid
    const auto beats = visibleBeats();
    const int perBar = juce::jmax(1, engine.beatsPerBar.load());
    for (double b = std::ceil(beats.getStart() / kSnap) * kSnap; b <= beats.getEnd(); b += kSnap)
    {
        const bool onBeat = std::abs(b - std::round(b)) < 1.0e-6;
        const bool onBar = onBeat && ((int) std::round(b)) % perBar == 0;
        g.setColour(onBar ? Theme::textDim.withAlpha(0.6f) : onBeat ? Theme::outline : Theme::outline.withAlpha(0.35f));
        g.drawVerticalLine((int) xForBeat(b), (float) area.getY(), (float) area.getBottom());
    }

    // Notes
    g.saveState();
    g.reduceClipRegion(area);
    for (auto* n : mc->getSequence().getNotes())
    {
        const float x0 = xForBeat(n->getStartBeat().inBeats());
        const float x1 = xForBeat(n->getEndBeat().inBeats());
        const float y = yForNote(n->getNoteNumber());
        auto r = juce::Rectangle<float>(x0, y + 0.5f, juce::jmax(3.0f, x1 - x0), (float) kRowHeight - 1.0f);
        const bool isSelected = n->state == selected;
        g.setColour(colour.withAlpha(0.35f + 0.65f * (float) n->getVelocity() / 127.0f));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(isSelected ? Theme::text : colour.brighter(0.4f));
        g.drawRoundedRectangle(r, 2.0f, isSelected ? 1.5f : 0.8f);
    }
    g.restoreState();

    // Playhead
    const auto playhead = engine.toTime(engine.getPosition());
    if (mc->getPosition().time.contains(playhead))
    {
        const float x = xForBeat(mc->getContentBeatAtTime(playhead).inBeats());
        g.setColour(Theme::text);
        g.drawVerticalLine((int) x, (float) area.getY(), (float) area.getBottom());
    }
}

void PianoRoll::resized()
{
    closeButton.setBounds(getLocalBounds().withHeight(kHeaderHeight).removeFromRight(76).reduced(4, 3));
}

//==============================================================================
void PianoRoll::mouseMove(const juce::MouseEvent& e)
{
    bool edge = false;
    const auto* n = gridArea().contains(e.getPosition()) ? noteAt(e.position, edge) : nullptr;
    setMouseCursor(n == nullptr ? juce::MouseCursor::NormalCursor
                   : edge       ? juce::MouseCursor::LeftRightResizeCursor
                                : juce::MouseCursor::DraggingHandCursor);
}

void PianoRoll::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::None;
    auto* mc = clip();
    if (mc == nullptr || !gridArea().contains(e.getPosition()))
        return;

    bool edge = false;
    auto* n = noteAt(e.position, edge);
    undo().beginNewTransaction("Edit notes");

    if (n == nullptr)
    {
        // A new note, which the drag then sizes.
        // Starts at the sixteenth the click falls in.
        const double clicked = beatAtX(e.position.x);
        const double start = juce::jmax(visibleBeats().getStart(),
                                        e.mods.isCommandDown() ? clicked : std::floor(clicked / kSnap) * kSnap);
        n = mc->getSequence().addNote(noteAtY(e.position.y), te::BeatPosition::fromBeats(start),
                                      te::BeatDuration::fromBeats(lastLength), 100, 0, &undo());
        if (n == nullptr)
            return;
        edge = true;
        project.markDirty();
    }

    selected = n->state;
    dragNote = n->state;
    dragStartBeat = n->getStartBeat().inBeats();
    dragLength = n->getLengthBeats().inBeats();
    dragPitch = n->getNoteNumber();
    dragDownBeat = beatAtX(e.position.x);
    dragDownNote = noteAtY(e.position.y);
    drag = edge ? Drag::Resize : Drag::Move;
    repaint();
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e)
{
    auto* mc = clip();
    if (mc == nullptr || drag == Drag::None)
        return;
    auto* n = mc->getSequence().getNoteFor(dragNote);
    if (n == nullptr)
        return;

    const double delta = beatAtX(e.position.x) - dragDownBeat;
    if (drag == Drag::Move)
    {
        const double start = juce::jmax(0.0, snap(dragStartBeat + delta, e.mods));
        const int pitch = juce::jlimit(0, 127, dragPitch + noteAtY(e.position.y) - dragDownNote);
        n->setStartAndLength(te::BeatPosition::fromBeats(start), te::BeatDuration::fromBeats(dragLength), &undo());
        if (pitch != n->getNoteNumber())
            n->setNoteNumber(pitch, &undo());
    }
    else
    {
        const double end = snap(dragStartBeat + dragLength + delta, e.mods);
        const double length = juce::jmax(e.mods.isCommandDown() ? kMinLength : kSnap, end - dragStartBeat);
        n->setStartAndLength(te::BeatPosition::fromBeats(dragStartBeat), te::BeatDuration::fromBeats(length), &undo());
        lastLength = length;
    }
    project.markDirty();
    repaint();
}

void PianoRoll::mouseUp(const juce::MouseEvent&)
{
    drag = Drag::None;
    undo().beginNewTransaction();
}

void PianoRoll::mouseDoubleClick(const juce::MouseEvent& e)
{
    auto* mc = clip();
    if (mc == nullptr)
        return;
    bool edge = false;
    if (auto* n = noteAt(e.position, edge))
    {
        undo().beginNewTransaction("Delete note");
        mc->getSequence().removeNote(*n, &undo());
        selected = {};
        project.markDirty();
        repaint();
    }
}

bool PianoRoll::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (auto* mc = clip())
            if (auto* n = mc->getSequence().getNoteFor(selected))
            {
                undo().beginNewTransaction("Delete note");
                mc->getSequence().removeNote(*n, &undo());
                selected = {};
                project.markDirty();
                repaint();
                return true;
            }
    }
    if (key == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }
    return false;
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int rows = juce::jmax(1, gridArea().getHeight() / kRowHeight);
    const int step = (int) std::round(wheel.deltaY * 24.0f);
    topNote = juce::jlimit(rows - 1, 127, topNote + (step != 0 ? step : (wheel.deltaY > 0 ? 1 : wheel.deltaY < 0 ? -1 : 0)));
    repaint();
}
