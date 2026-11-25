#include "PianoRoll.h"

namespace
{
    constexpr double epsilon = 1e-9;
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
    for (const int denominator : { 2, 4, 8, 16, 32 })
        subdivision.addItem("1/" + juce::String(denominator), denominator);
    subdivision.setSelectedId(16, juce::dontSendNotification);
    subdivision.setTooltip("Grid and quantize resolution");
    subdivision.onChange = [this] { grabKeyboardFocus(); repaint(); };
    addAndMakeVisible(subdivision);
    quantize.setTooltip("Snap selected note starts to the grid (Q)");
    quantize.onClick = [this] { quantizeNotes(); grabKeyboardFocus(); };
    addAndMakeVisible(quantize);
    expression.addItem("Velocity", 1);
    auto addController = [](juce::PopupMenu& menu, int cc)
    {
        auto name = juce::String("CC ") + juce::String(cc);
        if (const auto* label = juce::MidiMessage::getControllerName(cc))
            name << "  " << label;
        menu.addItem(cc + 2, name);
    };
    auto& expressionMenu = *expression.getRootMenu();
    expressionMenu.addSeparator();
    constexpr int commonControllers[] { 1, 7, 10, 11, 64, 74 };
    for (auto cc : commonControllers)
        addController(expressionMenu, cc);
    for (int first = 0; first < 120; first += 20)
    {
        juce::PopupMenu controllers;
        for (int cc = first; cc < first + 20; ++cc)
            if (std::find(std::begin(commonControllers), std::end(commonControllers), cc) == std::end(commonControllers))
                addController(controllers, cc);
        expressionMenu.addSubMenu("CC " + juce::String(first) + "–" + juce::String(first + 19), controllers);
    }
    expression.setSelectedId(1, juce::dontSendNotification);
    expression.setTooltip("Show note velocity or MIDI controller data below the notes");
    expression.onChange = [this] { grabKeyboardFocus(); repaint(); };
    addAndMakeVisible(expression);
    updateSelectionControls();
    setWantsKeyboardFocus(true);
}

void PianoRoll::setClip(juce::int64 id)
{
    clipId = id;
    selected.clear();
    selectionBeforeDrag.clear();
    movingNotes.clear();
    dragNote = {};
    dragController = {};
    marquee.reset();
    insertionBeat.reset();
    drag = Drag::None;
    updateSelectionControls();
    centreOnNotes();
    repaint();
}

void PianoRoll::refresh()
{
    auto* mc = clip();
    if (clipId != 0 && mc == nullptr)
    {
        // The clip went away (deleted, undone, or replaced by an edit that cut it).
        clipId = 0;
        if (onClose)
            onClose();
        return;
    }
    if (mc != nullptr)
        std::erase_if(selected, [&](const auto& note) { return mc->getSequence().getNoteFor(note) == nullptr; });
    updateSelectionControls();
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
    return getLocalBounds().withTrimmedTop(kHeaderHeight).withTrimmedLeft(kKeyboardWidth).withTrimmedBottom(kExpressionHeight);
}

juce::Rectangle<int> PianoRoll::expressionArea() const
{
    auto area = getLocalBounds().withTrimmedTop(kHeaderHeight);
    return area.removeFromBottom(kExpressionHeight).withTrimmedLeft(kKeyboardWidth);
}

float PianoRoll::yForValue(int value) const
{
    const auto area = expressionArea().reduced(0, 6);
    return static_cast<float>(area.getBottom()) - static_cast<float>(value) * static_cast<float>(area.getHeight()) / 127;
}

int PianoRoll::valueAtY(float y) const
{
    const auto area = expressionArea().reduced(0, 6);
    return juce::jlimit(0, 127, juce::roundToInt((static_cast<float>(area.getBottom()) - y) * 127
                                                / static_cast<float>(juce::jmax(1, area.getHeight()))));
}

juce::Range<double> PianoRoll::visibleBeats() const
{
    auto* mc = clip();
    if (mc == nullptr)
        return { 0.0, 4.0 };
    const auto pos = mc->getPosition();
    const double a = mc->getContentBeatAtTime(pos.getStart()).inBeats();
    const double b = mc->getContentBeatAtTime(pos.getEnd()).inBeats();
    return { a, juce::jmax(a + epsilon, b) };
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

double PianoRoll::gridStep() const
{
    return 4.0 / juce::jmax(2, subdivision.getSelectedId());
}

double PianoRoll::snap(double beat, const juce::ModifierKeys& mods) const
{
    const double step = gridStep();
    return mods.isCommandDown() ? beat : std::round(beat / step) * step;
}

double PianoRoll::editBeat(float x, const juce::ModifierKeys& mods) const
{
    const double clicked = beatAtX(x);
    const double step = gridStep();
    const auto visible = visibleBeats();
    return visible.clipValue(mods.isCommandDown() ? clicked : std::floor((clicked + epsilon) / step) * step);
}

juce::Rectangle<float> PianoRoll::noteBounds(const te::MidiNote& note) const
{
    const float x = xForBeat(note.getStartBeat().inBeats());
    const float right = xForBeat(note.getEndBeat().inBeats());
    return { x, yForNote(note.getNoteNumber()) + 0.5f, juce::jmax(3.0f, right - x), kRowHeight - 1.0f };
}

te::MidiNote* PianoRoll::noteAt(juce::Point<float> point, bool& onRightEdge) const
{
    onRightEdge = false;
    auto* mc = clip();
    if (mc == nullptr)
        return nullptr;
    // The last drawn note is on top when notes overlap.
    const auto& notes = mc->getSequence().getNotes();
    for (int i = notes.size(); --i >= 0;)
    {
        auto* note = notes[i];
        const auto bounds = noteBounds(*note);
        if (bounds.contains(point))
        {
            onRightEdge = point.x >= bounds.getRight() - juce::jmin(6.0f, bounds.getWidth() / 3.0f);
            return note;
        }
    }
    return nullptr;
}

bool PianoRoll::isSelected(const juce::ValueTree& note) const
{
    return std::find(selected.begin(), selected.end(), note) != selected.end();
}

void PianoRoll::updateSelectionControls()
{
    quantize.setEnabled(!selected.empty());
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
    g.drawText("Click to add  |  Right-drag to select  |  Right-click for note actions",
               getLocalBounds().withHeight(kHeaderHeight).withTrimmedLeft(220).withTrimmedRight(399),
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
    const double step = gridStep();
    const double pixelsPerBeat = (double) juce::jmax(1, area.getWidth()) / beats.getLength();
    double displayedStep = step;
    while (pixelsPerBeat * displayedStep < 3)
        displayedStep *= 2;
    for (double b = std::ceil(beats.getStart() / displayedStep) * displayedStep; b <= beats.getEnd(); b += displayedStep)
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
        const auto r = noteBounds(*n);
        const bool noteSelected = isSelected(n->state);
        g.setColour(colour.withAlpha(0.35f + 0.65f * (float) n->getVelocity() / 127.0f));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(noteSelected ? Theme::text : colour.brighter(0.4f));
        g.drawRoundedRectangle(r, 2.0f, noteSelected ? 1.5f : 0.8f);
    }
    if (marquee)
    {
        g.setColour(Theme::accent.withAlpha(0.14f));
        g.fillRect(*marquee);
        g.setColour(Theme::accent);
        g.drawRect(*marquee, 1.0f);
    }
    if (insertionBeat && !clipboard.empty())
    {
        const auto x = xForBeat(*insertionBeat);
        g.setColour(Theme::accent.withAlpha(0.65f));
        g.drawLine(x, (float) area.getY(), x, (float) area.getBottom(), 1.0f);
    }
    g.restoreState();

    paintExpression(g, *mc);

    // Playhead
    const auto playhead = engine.toTime(engine.getPosition());
    if (mc->getPosition().time.contains(playhead))
    {
        const float x = xForBeat(mc->getContentBeatAtTime(playhead).inBeats());
        g.setColour(Theme::text);
        g.drawVerticalLine((int) x, (float) area.getY(), (float) expressionArea().getBottom());
    }
}

void PianoRoll::resized()
{
    auto row = getLocalBounds().withHeight(kHeaderHeight);
    closeButton.setBounds(row.removeFromRight(76).reduced(4, 3));
    quantize.setBounds(row.removeFromRight(84).reduced(4, 3));
    subdivision.setBounds(row.removeFromRight(74).reduced(4, 3));
    expression.setBounds(row.removeFromRight(158).reduced(4, 3));
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
    if (isShowing())
        grabKeyboardFocus();
    drag = Drag::None;
    auto* mc = clip();
    if (mc == nullptr)
        return;
    if (expressionArea().contains(e.getPosition()))
    {
        expressionMouseDown(e);
        return;
    }
    if (!gridArea().contains(e.getPosition()))
        return;

    insertionBeat = editBeat(e.position.x, e.mods);
    if (e.mods.isPopupMenu())
    {
        drag = Drag::Select;
        selectionBeforeDrag = e.mods.isShiftDown() ? selected : std::vector<juce::ValueTree>();
        repaint();
        return;
    }

    bool edge = false;
    auto* note = noteAt(e.position, edge);
    undo().beginNewTransaction("Edit notes");
    if (note == nullptr)
    {
        const double start = *insertionBeat;
        const double length = juce::jmin(lastLength, visibleBeats().getEnd() - start);
        if (length <= epsilon)
            return;
        note = mc->getSequence().addNote(noteAtY(e.position.y), te::BeatPosition::fromBeats(start),
                                        te::BeatDuration::fromBeats(length), 100, 0, &undo());
        if (note == nullptr)
            return;
        edge = true;
        project.markDirty();
    }

    if (!isSelected(note->state))
        selected = { note->state };
    dragNote = note->state;
    dragStartBeat = note->getStartBeat().inBeats();
    dragLength = note->getLengthBeats().inBeats();
    dragDownBeat = beatAtX(e.position.x);
    dragDownNote = noteAtY(e.position.y);
    drag = edge ? Drag::Resize : Drag::Move;
    movingNotes.clear();
    if (drag == Drag::Move)
        for (const auto& state : selected)
            if (auto* item = mc->getSequence().getNoteFor(state))
                movingNotes.push_back({ state, item->getStartBeat().inBeats(), item->getLengthBeats().inBeats(), item->getNoteNumber() });
    updateSelectionControls();
    repaint();
}

void PianoRoll::updateMarquee(const juce::MouseEvent& e)
{
    const auto from = e.getMouseDownPosition().toFloat(), to = e.position;
    auto box = juce::Rectangle<float>::leftTopRightBottom(juce::jmin(from.x, to.x), juce::jmin(from.y, to.y),
                                                        juce::jmax(from.x, to.x), juce::jmax(from.y, to.y));
    marquee = box.getIntersection(gridArea().toFloat());
    selected = selectionBeforeDrag;
    if (auto* mc = clip())
        for (auto* note : mc->getSequence().getNotes())
            if (marquee->intersects(noteBounds(*note)) && !isSelected(note->state))
                selected.push_back(note->state);
    updateSelectionControls();
    repaint();
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e)
{
    if (drag == Drag::Velocity || drag == Drag::Controller)
    {
        editExpression(e);
        return;
    }
    if (drag == Drag::Select)
    {
        if (e.mouseWasDraggedSinceMouseDown())
            updateMarquee(e);
        return;
    }

    auto* mc = clip();
    if (mc == nullptr || drag == Drag::None)
        return;

    const double delta = beatAtX(e.position.x) - dragDownBeat;
    const auto visible = visibleBeats();
    if (drag == Drag::Move)
    {
        double beatShift = snap(dragStartBeat + delta, e.mods) - dragStartBeat;
        int pitchShift = noteAtY(e.position.y) - dragDownNote;
        double minShift = -std::numeric_limits<double>::infinity(), maxShift = -minShift;
        int minPitch = -127, maxPitch = 127;
        for (const auto& note : movingNotes)
        {
            minShift = juce::jmax(minShift, juce::jmin(0.0, visible.getStart() - note.start));
            maxShift = juce::jmin(maxShift, juce::jmax(0.0, visible.getEnd() - note.start - note.length));
            minPitch = juce::jmax(minPitch, -note.pitch);
            maxPitch = juce::jmin(maxPitch, 127 - note.pitch);
        }
        beatShift = juce::jlimit(minShift, maxShift, beatShift);
        pitchShift = juce::jlimit(minPitch, maxPitch, pitchShift);
        for (const auto& note : movingNotes)
            if (auto* item = mc->getSequence().getNoteFor(note.state))
            {
                item->setStartAndLength(te::BeatPosition::fromBeats(note.start + beatShift),
                                        te::BeatDuration::fromBeats(note.length), &undo());
                if (item->getNoteNumber() != note.pitch + pitchShift)
                    item->setNoteNumber(note.pitch + pitchShift, &undo());
            }
    }
    else if (auto* note = mc->getSequence().getNoteFor(dragNote))
    {
        const double end = snap(dragStartBeat + dragLength + delta, e.mods);
        const double maxLength = juce::jmax(epsilon, visible.getEnd() - dragStartBeat);
        const double minLength = juce::jmin(maxLength, e.mods.isCommandDown() ? kMinLength : gridStep());
        const double length = juce::jlimit(minLength, maxLength, end - dragStartBeat);
        note->setStartAndLength(te::BeatPosition::fromBeats(dragStartBeat), te::BeatDuration::fromBeats(length), &undo());
        lastLength = length;
    }
    project.markDirty();
    repaint();
}

void PianoRoll::mouseUp(const juce::MouseEvent& e)
{
    const auto finishedDrag = std::exchange(drag, Drag::None);
    if (finishedDrag == Drag::Select)
    {
        if (marquee)
        {
            updateMarquee(e);
            marquee.reset();
            repaint();
        }
        else if (gridArea().contains(e.getPosition()))
            showNoteMenu(e);
        selectionBeforeDrag.clear();
    }
    else if (finishedDrag != Drag::None)
        undo().beginNewTransaction();
}

void PianoRoll::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || !gridArea().contains(e.getPosition()))
        return;
    bool edge = false;
    if (auto* note = noteAt(e.position, edge))
    {
        selected = { note->state };
        deleteNotes();
    }
}

void PianoRoll::showNoteMenu(const juce::MouseEvent& e)
{
    auto* mc = clip();
    if (mc == nullptr)
        return;
    bool edge = false;
    if (auto* note = noteAt(e.position, edge))
    {
        if (e.mods.isShiftDown())
        {
            if (isSelected(note->state))
                std::erase(selected, note->state);
            else
                selected.push_back(note->state);
        }
        else if (!isSelected(note->state))
            selected = { note->state };
    }

    const double beat = editBeat(e.position.x, e.mods);
    insertionBeat = beat;
    updateSelectionControls();
    repaint();

    juce::PopupMenu menu;
    menu.addItem(1, "Copy Notes", !selected.empty());
    menu.addItem(2, "Paste Notes Here", canPasteAt(beat));
    if (!clipboard.empty() && !canPasteAt(beat))
        menu.addItem(6, "Copied notes do not fit here", false);
    menu.addSeparator();
    menu.addItem(3, "Quantize to " + subdivision.getText(), !selected.empty());
    menu.addItem(4, "Delete Selected Notes", !selected.empty());
    menu.addItem(5, "Select All Notes");

    auto clipState = mc->state;
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<PianoRoll>(this), beat, clipState](int result)
    {
        if (!safe || safe->clip() == nullptr || safe->clip()->state != clipState)
            return;
        switch (result)
        {
            case 1: safe->copyNotes(); break;
            case 2: safe->pasteNotes(beat); break;
            case 3: safe->quantizeNotes(); break;
            case 4: safe->deleteNotes(); break;
            case 5: safe->selectAllNotes(); break;
            default: break;
        }
    });
}

void PianoRoll::selectAllNotes()
{
    selected.clear();
    if (auto* mc = clip())
    {
        const auto beats = visibleBeats();
        for (auto* note : mc->getSequence().getNotes())
            if (note->getStartBeat().inBeats() < beats.getEnd() && note->getEndBeat().inBeats() > beats.getStart())
                selected.push_back(note->state);
    }
    updateSelectionControls();
    repaint();
}

juce::Range<double> PianoRoll::selectedBeatRange() const
{
    auto* mc = clip();
    if (mc == nullptr)
        return {};
    auto start = std::numeric_limits<double>::infinity();
    auto end = -std::numeric_limits<double>::infinity();
    for (const auto& state : selected)
        if (auto* note = mc->getSequence().getNoteFor(state))
        {
            start = juce::jmin(start, note->getStartBeat().inBeats());
            end = juce::jmax(end, note->getEndBeat().inBeats());
        }
    return start <= end ? juce::Range<double>(start, end) : juce::Range<double>();
}

std::vector<juce::ValueTree> PianoRoll::copySelectedNotes(double origin) const
{
    std::vector<juce::ValueTree> copies;
    if (auto* mc = clip())
        for (const auto& state : selected)
            if (auto* note = mc->getSequence().getNoteFor(state))
            {
                const auto relative = te::BeatPosition::fromBeats(note->getStartBeat().inBeats() - origin);
                copies.push_back(te::MidiNote::createNote(*note, relative, note->getLengthBeats()));
            }
    return copies;
}

void PianoRoll::copyNotes()
{
    auto copies = copySelectedNotes(selectedBeatRange().getStart());
    if (!copies.empty())
        clipboard = std::move(copies);
}

bool PianoRoll::canDuplicateNotes() const
{
    const auto selection = selectedBeatRange();
    return !selection.isEmpty() && selection.getEnd() + selection.getLength() <= visibleBeats().getEnd() + epsilon;
}

void PianoRoll::nudgeNotes(double beats, int semitones)
{
    auto* mc = clip();
    const auto selection = selectedBeatRange();
    if (mc == nullptr || selection.isEmpty())
        return;
    const auto visible = visibleBeats();
    const bool movingInTime = std::abs(beats) > epsilon;
    if (movingInTime && (selection.getStart() + beats < visible.getStart() - epsilon
                   || selection.getEnd() + beats > visible.getEnd() + epsilon))
        return;
    if (semitones != 0)
        for (const auto& state : selected)
            if (auto* note = mc->getSequence().getNoteFor(state))
                if (!juce::isPositiveAndBelow(note->getNoteNumber() + semitones, 128))
                    return;

    undo().beginNewTransaction("Move notes");
    for (const auto& state : selected)
        if (auto* note = mc->getSequence().getNoteFor(state))
        {
            if (movingInTime)
                note->setStartAndLength(note->getStartBeat() + te::BeatDuration::fromBeats(beats),
                                        note->getLengthBeats(), &undo());
            if (semitones != 0)
                note->setNoteNumber(note->getNoteNumber() + semitones, &undo());
        }
    undo().beginNewTransaction();
    project.markDirty();
    repaint();
}

double PianoRoll::latestPasteBeat() const
{
    double length = 0;
    for (const auto& state : clipboard)
        length = juce::jmax(length, te::MidiNote(state).getEndBeat().inBeats());
    return visibleBeats().getEnd() - length;
}

bool PianoRoll::canPasteAt(double beat) const
{
    return !clipboard.empty() && beat >= visibleBeats().getStart() - epsilon
        && beat <= latestPasteBeat() + epsilon;
}

double PianoRoll::pasteBeat() const
{
    if (insertionBeat)
        return *insertionBeat;
    const auto visible = visibleBeats();
    auto* mc = clip();
    if (mc == nullptr)
        return visible.getStart();
    const auto playhead = engine.toTime(engine.getPosition());
    return mc->getPosition().time.contains(playhead)
        ? visible.clipValue(snap(mc->getContentBeatAtTime(playhead).inBeats(), {}))
        : visible.getStart();
}

void PianoRoll::pasteNotes(double beat)
{
    if (canPasteAt(beat))
        insertNotes(clipboard, beat, "Paste notes");
}

void PianoRoll::insertNotes(const std::vector<juce::ValueTree>& notes, double beat, const juce::String& undoName)
{
    auto* mc = clip();
    if (mc == nullptr || notes.empty())
        return;

    undo().beginNewTransaction(undoName);
    selected.clear();
    for (const auto& state : notes)
    {
        const te::MidiNote original(state);
        const auto start = te::BeatPosition::fromBeats(beat + original.getStartBeat().inBeats());
        const te::MidiNote copy(te::MidiNote::createNote(original, start, original.getLengthBeats()));
        if (auto* inserted = mc->getSequence().addNote(copy, &undo()))
            selected.push_back(inserted->state);
    }
    undo().beginNewTransaction();
    insertionBeat = beat;
    project.markDirty();
    updateSelectionControls();
    repaint();
}

void PianoRoll::deleteNotes()
{
    auto* mc = clip();
    if (mc == nullptr || selected.empty())
        return;
    undo().beginNewTransaction("Delete notes");
    for (const auto& state : selected)
        if (auto* note = mc->getSequence().getNoteFor(state))
            mc->getSequence().removeNote(*note, &undo());
    undo().beginNewTransaction();
    selected.clear();
    project.markDirty();
    updateSelectionControls();
    repaint();
}

void PianoRoll::quantizeNotes()
{
    auto* mc = clip();
    if (mc == nullptr || selected.empty())
        return;

    const double step = gridStep();
    const auto beats = visibleBeats();
    const double first = std::ceil((beats.getStart() - epsilon) / step) * step;
    bool changed = false;
    undo().beginNewTransaction("Quantize notes");
    for (const auto& state : selected)
        if (auto* note = mc->getSequence().getNoteFor(state))
        {
            const auto length = note->getLengthBeats();
            const double last = std::floor((beats.getEnd() - length.inBeats() + epsilon) / step) * step;
            if (last < first)
                continue;

            const auto start = te::BeatPosition::fromBeats(juce::jlimit(first, last, snap(note->getStartBeat().inBeats(), {})));
            if (start != note->getStartBeat())
            {
                note->setStartAndLength(start, length, &undo());
                changed = true;
            }
        }
    undo().beginNewTransaction();
    if (changed)
        project.markDirty();
    repaint();
}

bool PianoRoll::keyPressed(const juce::KeyPress& key)
{
    if (const auto* binding = Shortcuts::find(Shortcuts::Context::PianoRoll, key);
        binding != nullptr && binding->context == Shortcuts::Context::PianoRoll)
    {
        performShortcut(binding->command);
        return true;
    }
    return false;
}

void PianoRoll::performShortcut(Shortcuts::Command command)
{
    using namespace Shortcuts;
    if (engine.getState() == AudioEngine::State::Recording && command != cmdCloseEditor && command != cmdCopy)
        return;
    switch (static_cast<int>(command))
    {
        case cmdSelectAll: selectAllNotes(); break;
        case cmdCopy: copyNotes(); break;
        case cmdPaste: pasteNotesAtCursor(); break;
        case cmdCut: cutNotes(); break;
        case cmdDelete: deleteNotes(); break;
        case cmdQuantize: quantizeNotes(); break;
        case cmdDuplicate:
            if (canDuplicateNotes())
            {
                const auto selection = selectedBeatRange();
                insertNotes(copySelectedNotes(selection.getStart()), selection.getEnd(), "Duplicate notes");
            }
            break;
        case cmdPrevious: nudgeNotes(-gridStep(), 0); break;
        case cmdNext: nudgeNotes(gridStep(), 0); break;
        case cmdPitchUp: nudgeNotes(0, 1); break;
        case cmdPitchDown: nudgeNotes(0, -1); break;
        case cmdOctaveUp: nudgeNotes(0, 12); break;
        case cmdOctaveDown: nudgeNotes(0, -12); break;
        case cmdCloseEditor: if (onClose) onClose(); break;
        default: break;
    }
}

void PianoRoll::paintExpression(juce::Graphics& g, te::MidiClip& midi)
{
    const auto area = expressionArea();
    juce::Graphics::ScopedSaveState restore(g);
    auto wholeLane = area.withLeft(0);
    g.setColour(Theme::laneA);
    g.fillRect(wholeLane);
    g.setColour(Theme::outline);
    g.drawHorizontalLine(area.getY(), 0, static_cast<float>(area.getRight()));
    g.setFont(Theme::font(10));
    g.setColour(Theme::textDim);
    g.drawText("127", wholeLane.removeFromLeft(kKeyboardWidth).removeFromTop(18), juce::Justification::centred);
    g.drawText("0", juce::Rectangle<int>(0, area.getBottom() - 18, kKeyboardWidth, 18), juce::Justification::centred);
    g.reduceClipRegion(area);

    const auto beats = visibleBeats();
    auto xAt = [&](double beat) { return xForBeat(beat); };
    const auto bottom = yForValue(0);
    const int cc = controllerNumber();
    if (cc < 0)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (auto* note : midi.getSequence().getNotes())
            {
                const auto start = note->getStartBeat().inBeats();
                if (!beats.contains(start) || isSelected(note->state) != (pass == 1))
                    continue;
                const auto y = yForValue(note->getVelocity());
                g.setColour(pass == 1 ? Theme::accent : Theme::accent.withAlpha(0.55f));
                g.fillRect(xAt(start) - 2.5f, y, 5.0f, juce::jmax(2.0f, bottom - y));
            }
        return;
    }

    float x = static_cast<float>(area.getX()), y = yForValue(te::MidiControllerEvent::getControllerDefautValue(cc) >> 7);
    g.setColour(Theme::solo.withAlpha(0.8f));
    for (auto* event : midi.getSequence().getControllerEvents())
    {
        if (event->getType() != cc)
            continue;
        const double beat = event->getBeatPosition().inBeats();
        if (beat >= beats.getEnd())
            break;
        const auto nextY = yForValue(event->getControllerValue() >> 7);
        if (beat < beats.getStart())
        {
            y = nextY;
            continue;
        }
        const auto nextX = xAt(beat);
        g.drawLine(x, y, nextX, y, 1.25f);
        g.drawLine(nextX, y, nextX, nextY, 1.25f);
        g.fillEllipse(nextX - 3, nextY - 3, 6, 6);
        x = nextX;
        y = nextY;
    }
    g.drawLine(x, y, static_cast<float>(area.getRight()), y, 1.25f);
}

te::MidiControllerEvent* PianoRoll::controllerAt(juce::Point<float> position) const
{
    if (auto* mc = clip())
    {
        const int cc = controllerNumber();
        const auto beats = visibleBeats();
        for (auto* event : mc->getSequence().getControllerEvents())
            if (event->getType() == cc && beats.contains(event->getBeatPosition().inBeats())
                && position.getDistanceFrom({ xForBeat(event->getBeatPosition().inBeats()),
                                              yForValue(event->getControllerValue() >> 7) }) <= 7)
                return event;
    }
    return nullptr;
}

void PianoRoll::expressionMouseDown(const juce::MouseEvent& e)
{
    auto* mc = clip();
    if (mc == nullptr)
        return;
    auto& sequence = mc->getSequence();
    const int cc = controllerNumber();
    if (cc < 0)
    {
        if (e.mods.isPopupMenu())
            return;
        te::MidiNote* hit = nullptr;
        float best = std::numeric_limits<float>::infinity();
        for (auto* note : sequence.getNotes())
        {
            const double beat = note->getStartBeat().inBeats();
            const float distance = std::abs(e.position.y - yForValue(note->getVelocity()));
            if (visibleBeats().contains(beat) && std::abs(e.position.x - xForBeat(beat)) <= 6 && distance < best)
            {
                hit = note;
                best = distance;
            }
        }
        if (hit == nullptr)
            return;
        if (!isSelected(hit->state))
            selected = { hit->state };
        updateSelectionControls();
        undo().beginNewTransaction("Edit velocity");
        drag = Drag::Velocity;
        velocityDragStartY = e.position.y;
        velocityBeforeDrag.clear();
        for (const auto& state : selected)
            if (auto* note = sequence.getNoteFor(state))
                velocityBeforeDrag.emplace_back(state, note->getVelocity());
        repaint();
        return;
    }

    auto* hit = controllerAt(e.position);
    if (e.mods.isPopupMenu())
    {
        if (hit != nullptr)
        {
            undo().beginNewTransaction("Delete control change");
            sequence.removeControllerEvent(*hit, &undo());
            undo().beginNewTransaction();
            project.markDirty();
            repaint();
        }
        return;
    }

    undo().beginNewTransaction("Edit control change");
    if (hit == nullptr)
    {
        const auto end = visibleBeats().getEnd();
        const auto beat = te::BeatPosition::fromBeats(juce::jmin(end - epsilon, editBeat(e.position.x, e.mods)));
        auto* existing = sequence.getControllerEventAt(beat, cc);
        if (existing != nullptr && existing->getBeatPosition() == beat)
        {
            hit = existing;
            hit->setControllerValue(valueAtY(e.position.y) << 7, &undo());
        }
        else
            hit = sequence.addControllerEvent(beat, cc, valueAtY(e.position.y) << 7, &undo());
        project.markDirty();
    }
    if (hit != nullptr)
    {
        dragController = hit->state;
        dragStartBeat = hit->getBeatPosition().inBeats();
        dragDownBeat = beatAtX(e.position.x);
        drag = Drag::Controller;
    }
    repaint();
}

void PianoRoll::editExpression(const juce::MouseEvent& e)
{
    auto* mc = clip();
    if (mc == nullptr)
        return;
    auto& sequence = mc->getSequence();
    if (drag == Drag::Velocity)
    {
        const auto delta = velocityDragStartY - e.position.y;
        const int amount = juce::roundToInt(delta * 127 / static_cast<float>(juce::jmax(1, expressionArea().reduced(0, 6).getHeight())));
        for (const auto& [state, velocity] : velocityBeforeDrag)
            if (auto* note = sequence.getNoteFor(state))
                note->setVelocity(juce::jlimit(1, 127, velocity + amount), &undo());
    }
    else
    {
        for (auto* point : sequence.getControllerEvents())
            if (point->state == dragController)
            {
                const auto visible = visibleBeats();
                const auto beat = te::BeatPosition::fromBeats(juce::jlimit(visible.getStart(), visible.getEnd() - epsilon,
                    snap(dragStartBeat + beatAtX(e.position.x) - dragDownBeat, e.mods)));
                auto* existing = sequence.getControllerEventAt(beat, point->getType());
                point->setControllerValue(valueAtY(e.position.y) << 7, &undo());
                if (existing == nullptr || existing == point || existing->getBeatPosition() != beat)
                    point->setBeatPosition(beat, &undo());
                break;
            }
    }
    project.markDirty();
    repaint();
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int rows = juce::jmax(1, gridArea().getHeight() / kRowHeight);
    const int step = (int) std::round(wheel.deltaY * 24.0f);
    topNote = juce::jlimit(rows - 1, 127, topNote + (step != 0 ? step : (wheel.deltaY > 0 ? 1 : wheel.deltaY < 0 ? -1 : 0)));
    repaint();
}
