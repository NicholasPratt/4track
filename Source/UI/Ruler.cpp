#include "Ruler.h"
#include "LyricEditorWindow.h"
#include "LyricRows.h"

namespace
{
    juce::String singleLine(juce::String text)
    {
        return text.replace("\r\n", " ").replaceCharacters("\r\n\t", "   ");
    }

    class SongNoteDialog final : public juce::AlertWindow
    {
    public:
        SongNoteDialog(const juce::String& title, const juce::String& text, bool chord, juce::Component* parent)
            : AlertWindow(title, chord ? "Type a symbol such as C, Am7, Fmaj7 or G/B." : juce::String(),
                          juce::MessageBoxIconType::NoIcon, parent)
        {
            editor.setSize(chord ? 360 : 480, 28);
            editor.setFont(Theme::font(15));
            editor.setEscapeAndReturnKeysConsumed(false);
            editor.setText(singleLine(text), false);
            editor.setSelectAllWhenFocused(true);
            addCustomComponent(&editor);
            addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
            addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        }

        ~SongNoteDialog() override { removeCustomComponent(0); }

        juce::TextEditor editor;
    };
}

Ruler::Ruler(AudioEngine& e, Project& p, TimelineView& v) : engine(e), project(p), view(v)
{
    setWantsKeyboardFocus(true);
}

juce::int64 Ruler::snapped(juce::int64 s, const juce::ModifierKeys& mods) const
{
    if (!view.snapOn || mods.isCommandDown())
        return s;
    return Grid::make(engine, view).snap(s);
}

void Ruler::paint(juce::Graphics& g)
{
    const auto width = (float) getWidth();
    const auto height = (float) getHeight();
    g.fillAll(Theme::panel);

    const auto grid = Grid::make(engine, view);
    const double spBar = grid.samplesPerBar();
    const double pxPerBar = spBar / view.samplesPerPixel;
    const double pxPerBeat = grid.samplesPerBeat / view.samplesPerPixel;

    int labelEvery = 1;
    while (pxPerBar * labelEvery < 44.0)
        labelEvery *= 2;

    const auto firstBar = (juce::int64) std::floor(view.viewStart / spBar);
    const auto lastBar = (juce::int64) std::ceil((double) view.sampleAt(width) / spBar);
    const float barArea = baseHeight - kPunchStripHeight - kSectionHeight;

    g.setFont(Theme::font(11.0f));
    for (auto bar = juce::jmax<juce::int64>(0, firstBar); bar <= lastBar; ++bar)
    {
        const float x = view.xFor((juce::int64) ((double) bar * spBar));
        if (bar % labelEvery == 0)
        {
            g.setColour(Theme::outline);
            g.drawVerticalLine((int) x, 4.0f, barArea);
            g.setColour(Theme::textDim);
            g.drawText(juce::String(bar + 1), (int) x + 4, 2, 40, (int) barArea - 4, juce::Justification::centredLeft, false);
        }
        else if (pxPerBar >= 6.0)
        {
            g.setColour(Theme::gridBar);
            g.drawVerticalLine((int) x, barArea - 7.0f, barArea);
        }

        if (pxPerBeat >= 10.0)
        {
            g.setColour(Theme::gridBar);
            for (int beat = 1; beat < grid.beatsPerBar; ++beat)
            {
                const float bx = view.xFor((juce::int64) ((double) bar * spBar + beat * grid.samplesPerBeat));
                g.drawVerticalLine((int) bx, barArea - 4.0f, barArea);
            }
        }
    }

    // Punch strip
    auto strip = juce::Rectangle<float>(0.0f, barArea, width, kPunchStripHeight);
    g.setColour(Theme::bg);
    g.fillRect(strip);
    const auto pin = engine.punchIn.load(), pout = engine.punchOut.load();
    if (pout > pin)
    {
        const auto mode = engine.getRangeMode();
        const bool on = mode != AudioEngine::RangeMode::Off;
        const auto colour = mode == AudioEngine::RangeMode::Loop ? Theme::monitor : Theme::record;
        auto r = strip.withX(view.xFor(pin)).withRight(view.xFor(pout)).reduced(0.0f, 2.0f);
        g.setColour(colour.withAlpha(on ? 0.85f : 0.3f));
        g.fillRoundedRectangle(r, 2.0f);
        if (r.getWidth() > 50.0f)
        {
            g.setColour(on ? juce::Colours::white : Theme::text.withAlpha(0.6f));
            g.setFont(Theme::font(9.5f, true));
            g.drawText(mode == AudioEngine::RangeMode::Loop ? "LOOP" : on ? "PUNCH" : "RANGE (OFF)", r, juce::Justification::centred, false);
        }
    }

    auto sectionStrip = juce::Rectangle<float>(0, baseHeight - kSectionHeight, width, kSectionHeight);
    g.setColour(Theme::bg);
    g.fillRect(sectionStrip);
    g.setFont(Theme::font(12.0f, true));
    auto* sections = project.getEdit().getArrangerTrack();
    if (sections == nullptr || sections->getClips().isEmpty())
    {
        g.setColour(Theme::textDim);
        g.drawText(engine.hasRange() ? "+ Name selected range" : "Drag a range above to name a section",
                   sectionStrip.reduced(5, 0), juce::Justification::centredLeft);
    }
    else for (auto* clip : sections->getClips())
    {
        auto range = clip->getPosition().time;
        auto area = sectionStrip.withX(view.xFor(engine.toSamples(range.getStart())))
                                .withRight(view.xFor(engine.toSamples(range.getEnd()))).reduced(1, 1);
        g.setColour(Theme::accent.withAlpha(0.35f));
        g.fillRoundedRectangle(area, 3);
        g.setColour(Theme::text);
        g.drawText(clip->getName(), area.getIntersection(sectionStrip).reduced(4, 0), juce::Justification::centredLeft);
    }

    if (songwritingExpanded)
    {
        paintSongNotes(g, Project::SongEntry::Chord);
        paintSongNotes(g, Project::SongEntry::Lyric);
    }

    // Playhead
    const float px = view.xFor(engine.getPosition());
    if (px >= -6.0f && px <= width + 6.0f)
    {
        juce::Path tri;
        tri.addTriangle(px - 5.0f, 0.0f, px + 5.0f, 0.0f, px, 8.0f);
        g.setColour(engine.getState() == AudioEngine::State::Recording ? Theme::record : Theme::playhead);
        g.fillPath(tri);
        g.drawVerticalLine((int) px, 0.0f, height);
    }

    g.setColour(Theme::outline);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, width);
}

Ruler::Drag Ruler::punchZoneAt(float x) const
{
    const auto pin = engine.punchIn.load(), pout = engine.punchOut.load();
    if (pout <= pin)
        return Drag::PunchNew;
    const float xi = view.xFor(pin), xo = view.xFor(pout);
    if (std::abs(x - xi) <= 5.0f) return Drag::PunchIn;
    if (std::abs(x - xo) <= 5.0f) return Drag::PunchOut;
    if (x > xi && x < xo)         return Drag::PunchMove;
    return Drag::PunchNew;
}

void Ruler::mouseMove(const juce::MouseEvent& e)
{
    setTooltip({});
    if (isSongwritingArea(e.position))
    {
        const auto kind = songLane(Project::SongEntry::Chord).contains(e.position)
                             ? Project::SongEntry::Chord : Project::SongEntry::Lyric;
        if (kind == Project::SongEntry::Lyric)
            if (auto* line = lyricAt(e.position); line != nullptr
                && lyricBounds(*line).withWidth(lyricHandleWidth).contains(e.position))
            {
                setTooltip("Drag to move lyric to another beat");
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
                return;
            }
        if (auto note = songNoteAt(e.position, kind))
            setTooltip(kind == Project::SongEntry::Chord && !ChordPreview::notesFor(note->text).isEmpty()
                ? note->text + " · Drag to a track to place MIDI notes" : note->text);
        else
            setTooltip(kind == Project::SongEntry::Chord ? "Double-click to add a chord" : "Double-click to add a lyric line");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }
    if (!inPunchStrip(e.position.y))
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    const auto zone = punchZoneAt(e.position.x);
    setMouseCursor(zone == Drag::PunchIn || zone == Drag::PunchOut ? juce::MouseCursor::LeftRightResizeCursor
                   : zone == Drag::PunchMove ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::IBeamCursor);
}

void Ruler::mouseDown(const juce::MouseEvent& e)
{
    draggedChord.reset();
    draggedLyric.reset();
    const auto s = snapped(view.sampleAt(e.position.x), e.mods);
    if (songwritingExpanded && e.position.y >= baseHeight)
    {
        drag = Drag::None;
        songNoteClick(e, e.position.y < baseHeight + chordHeight ? Project::SongEntry::Chord : Project::SongEntry::Lyric);
        return;
    }
    if (e.position.y >= baseHeight - kSectionHeight)
    {
        drag = Drag::None;
        sectionClick(e);
        return;
    }
    dragOrigIn = engine.punchIn.load();
    dragOrigOut = engine.punchOut.load();
    dragAnchor = s;

    if (inPunchStrip(e.position.y))
    {
        drag = punchZoneAt(e.position.x);
        return;
    }

    drag = Drag::Locate;
    engine.setPosition(juce::jmax<juce::int64>(0, s));
    repaint();
    if (onViewChanged) onViewChanged();
}

void Ruler::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedLyric)
    {
        if (songLane(Project::SongEntry::Lyric).contains(e.position) && e.mouseWasDraggedSinceMouseDown())
            draggedLyric->target = songBeatAt(e.position.x - draggedLyric->grabOffset);
        else
            draggedLyric->target.reset();
        repaint();
        return;
    }
    if (draggedChord && e.mouseWasDraggedSinceMouseDown())
    {
        auto chord = std::exchange(draggedChord, std::nullopt);
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            const auto font = Theme::font(13, true);
            const auto width = juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, chord->text)) + 18;
            juce::Image image(juce::Image::ARGB, width, 24, true);
            juce::Graphics graphics(image);
            graphics.setColour(Theme::panelLight);
            graphics.fillRoundedRectangle(image.getBounds().toFloat(), 4);
            graphics.setColour(Theme::text);
            graphics.setFont(font);
            graphics.drawText(chord->text, image.getBounds(), juce::Justification::centred);
            const juce::Array<juce::var> payload { chord->text, chord->beat.inBeats() };
            container->startDragging(juce::var(payload), this, juce::ScaledImage(image), false, nullptr, &e.source);
        }
        return;
    }
    const auto s = juce::jmax<juce::int64>(0, snapped(view.sampleAt(e.position.x), e.mods));

    switch (drag)
    {
        case Drag::Locate:
            engine.setPosition(s);
            break;
        case Drag::PunchNew:
            if (std::abs(e.getDistanceFromDragStartX()) < 3)
                return;
            engine.punchIn = juce::jmin(dragAnchor, s);
            engine.punchOut = juce::jmax(dragAnchor, s);
            if (engine.getRangeMode() == AudioEngine::RangeMode::Off && engine.getState() != AudioEngine::State::Recording)
                engine.setRangeMode(AudioEngine::RangeMode::Punch);
            break;
        case Drag::PunchIn:
            engine.punchIn = juce::jmin(s, engine.punchOut.load() - 1);
            break;
        case Drag::PunchOut:
            engine.punchOut = juce::jmax(s, engine.punchIn.load() + 1);
            break;
        case Drag::PunchMove:
        {
            const auto delta = juce::jmax(-dragOrigIn, s - dragAnchor);
            engine.punchIn = dragOrigIn + delta;
            engine.punchOut = dragOrigOut + delta;
            break;
        }
        case Drag::None:
            break;
    }

    repaint();
    if (onViewChanged) onViewChanged();
}

void Ruler::mouseUp(const juce::MouseEvent& e)
{
    if (auto lyric = std::exchange(draggedLyric, std::nullopt))
    {
        if (lyric->target && songLane(Project::SongEntry::Lyric).contains(e.position)
            && engine.getState() != AudioEngine::State::Recording)
        {
            project.moveLyric(lyric->entry, songBeatAt(e.position.x - lyric->grabOffset));
            if (onViewChanged) onViewChanged();
        }
        repaint();
        return;
    }
    if (auto chord = std::exchange(draggedChord, std::nullopt))
        engine.previewChord(chord->text);
    if (drag != Drag::None && drag != Drag::Locate
        && (engine.punchIn.load() != dragOrigIn || engine.punchOut.load() != dragOrigOut))
    {
        project.markDirty();
        if (onPunchChanged) onPunchChanged();
    }
    drag = Drag::None;
}

void Ruler::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (inPunchStrip(e.position.y) && engine.punchOut.load() > engine.punchIn.load())
    {
        if (engine.getState() != AudioEngine::State::Recording)
            engine.setRangeMode(AudioEngine::nextRangeMode(engine.getRangeMode()));
        project.markDirty();
        if (onPunchChanged) onPunchChanged();
        repaint();
    }
}

void Ruler::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Scrolling over the ruler zooms, like most DAWs.
    view.zoomAround(e.position.x, std::pow(0.85, wheel.deltaY * 6.0 + wheel.deltaX * 0.0));
    if (onViewChanged) onViewChanged();
}

void Ruler::mouseMagnify(const juce::MouseEvent& e, float scale)
{
    view.zoomAround(e.position.x, 1.0 / juce::jmax(0.1f, scale));
    if (onViewChanged) onViewChanged();
}

juce::Rectangle<float> Ruler::songLane(Project::SongEntry kind) const
{
    const bool chord = kind == Project::SongEntry::Chord;
    return { 0, static_cast<float>(chord ? baseHeight : baseHeight + chordHeight),
             static_cast<float>(getWidth()), static_cast<float>(chord ? chordHeight : visibleLyricRows * lyricRowHeight) };
}

void Ruler::layoutLyrics(int width, int rowLimit)
{
    lyricLines.clear();
    lyricRowCount = 1;
    if (songwritingExpanded && width > 0)
    {
        auto notes = project.songNotes(Project::SongEntry::Lyric);
        const auto& tempo = project.getEdit().tempoSequence;
        const auto font = Theme::font(13);
        std::vector<juce::Range<float>> spans;
        for (auto& note : notes)
        {
            const float x = view.xFor(engine.toSamples(tempo.toTime(note.beat)));
            if (x >= static_cast<float>(width))
                break;
            note.text = singleLine(note.text);
            const float right = x + std::ceil(juce::GlyphArrangement::getStringWidth(font, note.text)) + lyricHandleWidth + 12.0f;
            if (right <= 0.0f)
                continue;
            spans.emplace_back(x, right);
            lyricLines.push_back({ std::move(note), spans.back(), 0 });
        }

        const auto rows = lyricRowsFor(spans);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            lyricLines[i].row = rows[i];
            lyricRowCount = juce::jmax(lyricRowCount, rows[i] + 1);
        }
    }
    visibleLyricRows = juce::jmin(rowLimit, lyricRowCount);
    setFirstLyricRow(firstLyricRow);
}

void Ruler::setFirstLyricRow(int row)
{
    firstLyricRow = juce::jlimit(0, lyricRowCount - visibleLyricRows, row);
    repaint();
}

juce::Rectangle<float> Ruler::lyricBounds(const LyricLine& line) const
{
    const auto y = baseHeight + chordHeight + (line.row - firstLyricRow) * lyricRowHeight;
    return { line.span.getStart(), static_cast<float>(y + 2), line.span.getLength(),
             static_cast<float>(lyricRowHeight - 4) };
}

juce::Rectangle<float> Ruler::chordBounds(const std::vector<Project::SongNote>& notes, size_t index) const
{
    const auto& tempo = project.getEdit().tempoSequence;
    const auto lane = songLane(Project::SongEntry::Chord);
    const float x = view.xFor(engine.toSamples(tempo.toTime(notes[index].beat)));
    const float available = index + 1 < notes.size()
        ? view.xFor(engine.toSamples(tempo.toTime(notes[index + 1].beat))) - x
        : lane.getRight() - x;
    const auto width = 12.0f + juce::GlyphArrangement::getStringWidth(Theme::font(13, true), notes[index].text);
    return { x, lane.getY() + 3, juce::jmax(0.0f, juce::jmin(width, available - 2)), lane.getHeight() - 6 };
}

const Ruler::LyricLine* Ruler::lyricAt(juce::Point<float> point) const
{
    for (const auto& line : lyricLines)
        if (lyricBounds(line).contains(point))
            return &line;
    return nullptr;
}

std::optional<Project::SongNote> Ruler::songNoteAt(juce::Point<float> position, Project::SongEntry kind) const
{
    if (kind == Project::SongEntry::Lyric)
    {
        if (auto* line = lyricAt(position))
            return line->note;
        return {};
    }

    const auto notes = project.songNotes(kind);
    for (size_t i = 0; i < notes.size(); ++i)
        if (chordBounds(notes, i).contains(position))
            return notes[i];
    return {};
}

void Ruler::paintSongNotes(juce::Graphics& g, Project::SongEntry kind)
{
    const auto lane = songLane(kind);
    const auto& tempo = project.getEdit().tempoSequence;
    const bool chord = kind == Project::SongEntry::Chord;
    const auto notes = chord ? project.songNotes(kind) : std::vector<Project::SongNote>();
    g.setColour(Theme::bg);
    g.fillRect(lane);
    g.setColour(Theme::outline);
    g.drawHorizontalLine(static_cast<int>(lane.getY()), 0, lane.getWidth());

    const auto firstBeat = tempo.toBeats(engine.toTime(view.sampleAt(0))).inBeats();
    const auto lastBeat = tempo.toBeats(engine.toTime(view.sampleAt(lane.getWidth()))).inBeats();
    if (lastBeat - firstBeat < lane.getWidth() / 8.0)
        for (auto beat = std::ceil(firstBeat); beat <= lastBeat; beat += 1)
        {
            const auto x = view.xFor(engine.toSamples(tempo.toTime(te::BeatPosition::fromBeats(beat))));
            g.setColour(Theme::gridBar);
            g.drawVerticalLine(static_cast<int>(x), lane.getY() + 1, lane.getBottom());
        }

    g.setFont(Theme::font(13, chord));
    if (chord ? notes.empty() : lyricLines.empty())
    {
        g.setColour(Theme::textDim);
        g.drawText(chord ? "Double-click a beat to add a chord" : "Double-click a beat to add a lyric line",
                   lane.reduced(8, 0), juce::Justification::centredLeft);
    }

    juce::Graphics::ScopedSaveState restore(g);
    g.reduceClipRegion(lane.toNearestInt());
    auto drawLine = [&](juce::Rectangle<float> area, const juce::String& line)
    {
        if (!area.intersects(lane))
            return;
        g.setColour((chord ? Theme::monitor : Theme::accent).withAlpha(0.25f));
        g.fillRoundedRectangle(area, 3);
        g.setColour(Theme::text);
        if (!chord)
        {
            const auto handle = area.removeFromLeft(lyricHandleWidth);
            g.setColour(Theme::textDim);
            for (int row = -1; row <= 1; ++row)
                for (int col = 0; col < 2; ++col)
                    g.fillEllipse(handle.getCentreX() - 3.0f + 4.0f * static_cast<float>(col),
                                  handle.getCentreY() - 1.0f + 4.0f * static_cast<float>(row), 2, 2);
            g.setColour(Theme::text);
        }
        const auto text = area.reduced(5, 1).toNearestInt();
        if (!text.isEmpty())
            g.drawText(line, text, juce::Justification::centredLeft, chord);
    };

    if (chord)
        for (size_t i = 0; i < notes.size(); ++i)
            drawLine(chordBounds(notes, i), notes[i].text);
    else
        for (const auto& line : lyricLines)
            drawLine(lyricBounds(line), line.note.text);

    const auto dropBeat = lyricDropBeat ? lyricDropBeat : draggedLyric ? draggedLyric->target : std::nullopt;
    if (!chord && dropBeat)
    {
        const auto x = view.xFor(engine.toSamples(tempo.toTime(*dropBeat)));
        g.setColour(Theme::accent);
        g.drawLine(x, lane.getY(), x, lane.getBottom(), 2);
    }

    if (hasKeyboardFocus(false) && hasSongwritingCursor() && songCursor->kind == kind)
    {
        g.setColour(Theme::accent);
        if (chord && songCursor->entry.isValid())
            for (size_t index = 0; index < notes.size(); ++index)
                if (notes[index].entry == songCursor->entry)
                {
                    g.drawRoundedRectangle(chordBounds(notes, index).reduced(0.5f), 3, 1);
                    return;
                }
        if (!chord && songCursor->entry.isValid())
            for (const auto& line : lyricLines)
                if (line.note.entry == songCursor->entry)
                {
                    g.drawRoundedRectangle(lyricBounds(line).reduced(0.5f), 3, 1);
                    return;
                }
        const auto x = view.xFor(engine.toSamples(tempo.toTime(songCursor->beat)));
        g.drawLine(x, lane.getY() + 2, x, lane.getBottom() - 2, 2);
    }
}

te::BeatPosition Ruler::songBeatAt(float x) const
{
    const auto clicked = project.getEdit().tempoSequence.toBeats(engine.toTime(view.sampleAt(x)));
    return te::BeatPosition::fromBeats(juce::jmax(0.0, std::round(clicked.inBeats())));
}

bool Ruler::isInterestedInDragSource(const SourceDetails& source)
{
    const auto* line = dynamic_cast<const LyricLineDrag*>(source.description.getObject());
    return line != nullptr && line->edit == project.getEdit().state && songwritingExpanded
        && engine.getState() != AudioEngine::State::Recording;
}

std::optional<te::BeatPosition> Ruler::lyricDropAt(const SourceDetails& source)
{
    if (isInterestedInDragSource(source) && songLane(Project::SongEntry::Lyric).contains(source.localPosition.toFloat()))
        return songBeatAt(static_cast<float>(source.localPosition.x));
    return {};
}

void Ruler::itemDragEnter(const SourceDetails& source)
{
    itemDragMove(source);
}

void Ruler::itemDragMove(const SourceDetails& source)
{
    lyricDropBeat = lyricDropAt(source);
    repaint();
}

void Ruler::itemDragExit(const SourceDetails&)
{
    lyricDropBeat.reset();
    repaint();
}

void Ruler::itemDropped(const SourceDetails& source)
{
    lyricDropBeat.reset();
    if (const auto beat = lyricDropAt(source))
    {
        const auto& line = *static_cast<const LyricLineDrag*>(source.description.getObject());
        project.setSongNote(Project::SongEntry::Lyric, *beat, line.text);
        if (onViewChanged) onViewChanged();
    }
    repaint();
}

bool Ruler::hasSongwritingCursor() const
{
    return songwritingExpanded && songCursor && songCursor->edit == project.getEdit().state;
}

std::optional<Project::SongNote> Ruler::selectedSongNote() const
{
    if (hasSongwritingCursor() && songCursor->entry.isValid())
        for (const auto& note : project.songNotes(songCursor->kind))
            if (note.entry == songCursor->entry)
                return note;
    return {};
}

bool Ruler::canPasteSongText() const
{
    return hasSongwritingCursor() && singleLine(juce::SystemClipboard::getTextFromClipboard()).trim().isNotEmpty();
}

void Ruler::performSongShortcut(Shortcuts::Command command)
{
    using namespace Shortcuts;
    if (!hasSongwritingCursor())
        return;
    if (engine.getState() == AudioEngine::State::Recording
        && command != cmdCopy && command != cmdPrevious && command != cmdNext)
        return;

    auto& cursor = *songCursor;
    const auto kind = cursor.kind;
    const auto note = selectedSongNote();
    const auto beat = note ? note->beat : cursor.beat;
    switch (static_cast<int>(command))
    {
        case cmdPrevious:
        case cmdNext:
        {
            cursor.beat = te::BeatPosition::fromBeats(juce::jmax(0.0, beat.inBeats() + (command == cmdPrevious ? -1.0 : 1.0)));
            cursor.entry = {};
            const auto sample = engine.toSamples(project.getEdit().tempoSequence.toTime(cursor.beat));
            const auto x = view.xFor(sample);
            if (x < 0 || x >= static_cast<float>(getWidth()))
            {
                view.viewStart = juce::jmax(0.0, static_cast<double>(sample) - getWidth() * view.samplesPerPixel * 0.5);
                if (onViewChanged) onViewChanged();
            }
            break;
        }
        case cmdEditSongNote:
            if (note)
                editSongNote(kind, beat, note->text, note->entry);
            else if (kind == Project::SongEntry::Chord)
            {
                const auto chords = project.songNotes(kind);
                const auto existing = std::find_if(chords.begin(), chords.end(), [beat](const auto& chord) { return chord.beat == beat; });
                if (existing != chords.end())
                    editSongNote(kind, beat, existing->text, existing->entry);
                else
                    editSongNote(kind, beat, {});
            }
            else
                editSongNote(kind, beat, {});
            break;
        case cmdCopy:
        case cmdCut:
        case cmdDelete:
            if (note)
            {
                if (command != cmdDelete)
                    juce::SystemClipboard::copyTextToClipboard(note->text);
                if (command != cmdCopy)
                {
                    cursor.beat = beat;
                    project.setSongNote(kind, beat, {}, note->entry);
                }
            }
            break;
        case cmdPaste:
        {
            const auto text = singleLine(juce::SystemClipboard::getTextFromClipboard()).trim();
            if (text.isNotEmpty())
                project.setSongNote(kind, beat, text);
            break;
        }
        default: break;
    }
    repaint();
}

void Ruler::songNoteClick(const juce::MouseEvent& e, Project::SongEntry kind)
{
    const bool chord = kind == Project::SongEntry::Chord;
    const auto note = songNoteAt(e.position, kind);
    const auto beat = songBeatAt(e.position.x);
    songCursor = SongCursor { kind, note ? note->beat : beat, project.getEdit().state, note ? note->entry : juce::ValueTree() };
    if (isShowing())
        grabKeyboardFocus();
    repaint();
    enum Action { add = 1, editEntry, removeEntry, moveUp, moveDown, mergeAbove, mergeBelow, copyLyric, pasteLyric };
    if (e.mods.isPopupMenu())
    {
        if (engine.getState() == AudioEngine::State::Recording)
            return;
        juce::PopupMenu menu;
        menu.addItem(add, chord ? "Add chord at this beat…" : "Add lyric line at this beat…");
        juce::String clipboardLyric;
        if (!chord)
        {
            clipboardLyric = singleLine(juce::SystemClipboard::getTextFromClipboard()).trim();
            menu.addItem(copyLyric, "Copy lyric line", note.has_value());
            menu.addItem(pasteLyric, "Paste lyric at this beat", clipboardLyric.isNotEmpty());
        }
        if (note)
        {
            menu.addItem(editEntry, chord ? "Edit chord…" : "Edit lyric line…");
            menu.addItem(removeEntry, chord ? "Remove chord" : "Remove lyric line");
            if (!chord)
            {
                const bool hasAbove = project.adjacentLyric(note->entry, -1).isValid();
                const bool hasBelow = project.adjacentLyric(note->entry, 1).isValid();
                if (hasAbove || hasBelow)
                {
                    menu.addSeparator();
                    menu.addItem(moveUp, "Move up", hasAbove);
                    menu.addItem(moveDown, "Move down", hasBelow);
                    menu.addItem(mergeAbove, "Merge with line above", hasAbove);
                    menu.addItem(mergeBelow, "Merge with line below", hasBelow);
                }
            }
        }
        auto edit = project.getEdit().state;
        menu.showMenuAsync({}, [safe = juce::Component::SafePointer<Ruler>(this), kind, beat, note, edit, clipboardLyric](int result)
        {
            if (!safe || safe->project.getEdit().state != edit || safe->engine.getState() == AudioEngine::State::Recording)
                return;
            if (result == add)
            {
                if (kind == Project::SongEntry::Chord)
                    for (const auto& existing : safe->project.songNotes(kind))
                        if (existing.beat == beat)
                        {
                            safe->editSongNote(kind, beat, existing.text, existing.entry);
                            return;
                        }
                safe->editSongNote(kind, beat, {});
                return;
            }

            if (result == pasteLyric)
            {
                safe->project.setSongNote(Project::SongEntry::Lyric, beat, clipboardLyric);
                return;
            }

            if (!note)
                return;
            auto& song = safe->project;
            switch (result)
            {
                case copyLyric: juce::SystemClipboard::copyTextToClipboard(note->text); break;
                case editEntry: safe->editSongNote(kind, note->beat, note->text, note->entry); break;
                case removeEntry: song.setSongNote(kind, note->beat, {}, note->entry); break;
                case moveUp: song.reorderLyric(note->entry, -1); break;
                case moveDown: song.reorderLyric(note->entry, 1); break;
                case mergeAbove: song.mergeLyric(note->entry, -1); break;
                case mergeBelow: song.mergeLyric(note->entry, 1); break;
                default: break;
            }
        });
        return;
    }
    if (!chord && e.getNumberOfClicks() == 1 && engine.getState() != AudioEngine::State::Recording)
        if (auto* line = lyricAt(e.position); line != nullptr
            && lyricBounds(*line).withWidth(lyricHandleWidth).contains(e.position))
        {
            draggedLyric = LyricDrag { line->note.entry, e.position.x - line->span.getStart(), {} };
            return;
        }
    if (e.getNumberOfClicks() == 2 && engine.getState() != AudioEngine::State::Recording)
    {
        // Chord symbols and lyric boxes remain editable wherever they are clicked.
        // The context menu can add a lyric at the beat beneath an existing line.
        if (note)
            editSongNote(kind, note->beat, note->text, note->entry);
        else
            editSongNote(kind, beat, {});
        return;
    }
    if (chord && note)
    {
        if (engine.getState() != AudioEngine::State::Recording && !ChordPreview::notesFor(note->text).isEmpty())
            draggedChord = note;
        else
            engine.previewChord(note->text);
    }
}

void Ruler::editSongNote(Project::SongEntry kind, te::BeatPosition beat, juce::String text, juce::ValueTree entry)
{
    const bool chord = kind == Project::SongEntry::Chord;
    const auto position = engine.toSamples(project.getEdit().tempoSequence.toTime(beat));
    const auto title = juce::String(chord ? "Chord at " : "Lyrics at ") + Grid::make(engine, view).barsBeats(position);
    auto* dialog = new SongNoteDialog(title, text, chord, this);
    const auto edit = project.getEdit().state;
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<Ruler>(this), dialog, kind, beat, edit, entry](int result)
        {
            if (safe && result == 1 && safe->project.getEdit().state == edit
                && safe->engine.getState() != AudioEngine::State::Recording)
            {
                const auto value = singleLine(dialog->editor.getText());
                safe->project.setSongNote(kind, beat, value, entry);
                if (kind == Project::SongEntry::Chord)
                    safe->engine.previewChord(value);
                safe->repaint();
            }
        }), true);
}

void Ruler::sectionClick(const juce::MouseEvent& e)
{
    if (engine.getState() == AudioEngine::State::Recording)
        return;
    te::ArrangerClip::Ptr section;
    if (auto* track = project.getEdit().getArrangerTrack())
        for (auto* clip : track->getClips())
            if (clip->getPosition().time.contains(engine.toTime(view.sampleAt(e.position.x))))
                section = dynamic_cast<te::ArrangerClip*>(clip);

    if (section && !e.mods.isPopupMenu())
    {
        engine.setPosition(engine.toSamples(section->getPosition().getStart()));
        if (onViewChanged) onViewChanged();
        return;
    }
    if (engine.getState() != AudioEngine::State::Stopped)
        return;
    const te::TimeRange range {engine.toTime(engine.punchIn.load()), engine.toTime(juce::jmax(engine.punchIn.load(), engine.punchOut.load()))};
    if (!e.mods.isPopupMenu())
    {
        if (engine.hasRange()) nameSection(range);
        return;
    }
    juce::PopupMenu menu;
    menu.addItem(1, "Name selected range...", engine.hasRange());
    if (section)
    {
        menu.addItem(2, "Duplicate " + section->getName());
        menu.addItem(3, "Remove section label");
    }
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<Ruler>(this), id = section ? section->itemID : te::EditItemID(), range](int result)
    {
        if (!safe || safe->engine.getState() != AudioEngine::State::Stopped)
            return;
        if (result == 1) safe->nameSection(range);
        if (!id.isValid()) return;
        if (auto* clip = dynamic_cast<te::ArrangerClip*>(te::findClipForID(safe->project.getEdit(), id)))
        {
            if (result == 2) safe->project.duplicateSection(*clip);
            if (result == 3)
            {
                safe->project.getEdit().getUndoManager().beginNewTransaction("Remove section label");
                clip->removeFromParent();
                safe->project.getEdit().getUndoManager().beginNewTransaction();
            }
        }
        safe->repaint();
    });
}

void Ruler::nameSection(te::TimeRange range)
{
    auto* dialog = new juce::AlertWindow("Name song section", {}, juce::MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor("name", {}, "Section name");
    dialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<Ruler>(this), dialog, range](int result)
    {
        if (safe && result == 1)
        {
            auto added = safe->project.addSection(dialog->getTextEditorContents("name"), range);
            if (added.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Song section", added.getErrorMessage());
            safe->repaint();
        }
    }), true);
}
