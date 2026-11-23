#include "TrackLanes.h"

TrackLanes::TrackLanes(AudioEngine& e, Project& p, TimelineView& v) : engine(e), project(p), view(v)
{
    setWantsKeyboardFocus(false);
}

void TrackLanes::selectAll()
{
    selection.clear();
    for (auto& lane : project.getArrangement().tracks)
        for (auto& c : lane)
            selection.insert(c.id);
    repaint();
}

void TrackLanes::pruneSelection()
{
    const auto& arr = project.getArrangement();
    for (auto it = selection.begin(); it != selection.end();)
        it = arr.findClip(*it) == nullptr ? selection.erase(it) : std::next(it);
}

juce::int64 TrackLanes::snapped(juce::int64 s, const juce::ModifierKeys& mods) const
{
    if (!view.snapOn || mods.isCommandDown())
        return s;
    return Grid::make(engine, view).snap(s);
}

juce::Rectangle<float> TrackLanes::clipRect(const Clip& c, int track) const
{
    const float x0 = view.xFor(c.start);
    const float x1 = view.xFor(c.end());
    const float y = (float) (track * kTrackHeight - scrollY) + 3.0f;
    return { x0, y, juce::jmax(1.0f, x1 - x0), (float) kTrackHeight - 6.0f };
}

TrackLanes::Hit TrackLanes::findHit(juce::Point<float> p) const
{
    Hit hit;
    hit.track = trackAtY(p.y);
    const auto& arr = project.getArrangement();

    for (auto& c : arr.tracks[(size_t) hit.track])
    {
        auto r = clipRect(c, hit.track);
        if (!r.contains(p))
            continue;

        hit.clipId = c.id;
        const float edge = juce::jmin(6.0f, r.getWidth() / 4.0f);
        const float fadeInX = view.xFor(c.start + c.fadeIn);
        const float fadeOutX = view.xFor(c.end() - c.fadeOut);

        if (p.y < r.getY() + 12.0f && std::abs(p.x - fadeInX) <= 6.0f && p.x < r.getCentreX())
            hit.zone = Zone::FadeIn;
        else if (p.y < r.getY() + 12.0f && std::abs(p.x - fadeOutX) <= 6.0f && p.x > r.getCentreX())
            hit.zone = Zone::FadeOut;
        else if (p.x < r.getX() + edge)
            hit.zone = Zone::TrimStart;
        else if (p.x > r.getRight() - edge)
            hit.zone = Zone::TrimEnd;
        else
            hit.zone = Zone::Body;
        return hit;
    }
    return hit;
}

//==============================================================================
void TrackLanes::paint(juce::Graphics& g)
{
    const auto width = (float) getWidth();
    const auto& arr = preview ? *preview : project.getArrangement();

    // Lanes
    for (int t = 0; t < kNumTracks; ++t)
    {
        const float y = (float) (t * kTrackHeight - scrollY);
        auto lane = juce::Rectangle<float>(0.0f, y, width, (float) kTrackHeight);
        g.setColour(t % 2 == 0 ? Theme::laneA : Theme::laneB);
        g.fillRect(lane);
        if (engine.getTrack(t).armed.load())
        {
            g.setColour(Theme::record.withAlpha(0.06f));
            g.fillRect(lane);
        }
        g.setColour(Theme::bg);
        g.drawHorizontalLine((int) lane.getBottom() - 1, 0.0f, width);
    }

    const float contentBottom = (float) (getContentHeight() - scrollY);
    paintGrid(g, 0.0f, contentBottom);

    // Punch range overlay
    const auto pin = engine.punchIn.load(), pout = engine.punchOut.load();
    if (pout > pin)
    {
        auto r = juce::Rectangle<float>(view.xFor(pin), 0.0f, view.xFor(pout) - view.xFor(pin), contentBottom);
        g.setColour(Theme::record.withAlpha(engine.punchOn.load() ? 0.08f : 0.03f));
        g.fillRect(r);
        g.setColour(Theme::record.withAlpha(engine.punchOn.load() ? 0.5f : 0.15f));
        g.drawVerticalLine((int) r.getX(), 0.0f, contentBottom);
        g.drawVerticalLine((int) r.getRight(), 0.0f, contentBottom);
    }

    // Clips
    for (int t = 0; t < kNumTracks; ++t)
        for (auto& c : arr.tracks[(size_t) t])
        {
            auto r = clipRect(c, t);
            if (r.getRight() < 0.0f || r.getX() > width || r.getBottom() < 0.0f || r.getY() > (float) getHeight())
                continue;
            paintClip(g, c, t, selection.count(c.id) > 0, hover.clipId == c.id && !dragActive);
        }

    paintLiveTakes(g);

    if (arr.isEmpty() && engine.getRecordSession() == nullptr)
    {
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(14.0f));
        g.drawText("Arm a track (R) and press Record, or drop audio files here",
                   getLocalBounds().withHeight(juce::jmin(getHeight(), getContentHeight() - scrollY)),
                   juce::Justification::centred, false);
    }

    // Playhead
    const float px = view.xFor(engine.getPosition());
    if (px >= 0.0f && px <= width)
    {
        g.setColour(engine.getState() == AudioEngine::State::Recording ? Theme::record : Theme::playhead);
        g.drawVerticalLine((int) px, 0.0f, contentBottom);
    }
}

void TrackLanes::paintGrid(juce::Graphics& g, float top, float bottom)
{
    const auto grid = Grid::make(engine, view);
    const double first = std::floor(view.viewStart / grid.step) * grid.step;
    const double last = (double) view.sampleAt((float) getWidth());
    const double spBar = grid.samplesPerBar();

    for (double s = juce::jmax(0.0, first); s <= last; s += grid.step)
    {
        const bool isBar = std::fmod(s + 0.5, spBar) < 1.0;
        const bool isBeat = std::fmod(s + 0.5, grid.samplesPerBeat) < 1.0;
        g.setColour(isBar ? Theme::gridBar : isBeat ? Theme::gridBeat : Theme::gridBeat.withAlpha(0.5f));
        g.drawVerticalLine((int) view.xFor((juce::int64) s), top, bottom);
    }
}

template <typename MinMaxFn>
void TrackLanes::paintWaveform(juce::Graphics& g, juce::Rectangle<float> area, juce::int64 clipStart,
                               MinMaxFn&& minMaxForRange, juce::Colour colour, const Clip* envelope)
{
    const int x0 = juce::jmax(0, (int) std::floor(area.getX()));
    const int x1 = juce::jmin(getWidth(), (int) std::ceil(area.getRight()));
    const float mid = area.getCentreY();
    const float half = area.getHeight() * 0.5f;
    g.setColour(colour);

    for (int x = x0; x < x1; ++x)
    {
        const auto a = view.sampleAt((float) x) - clipStart;
        const auto b = juce::jmax(a + 1, view.sampleAt((float) x + 1.0f) - clipStart);
        float lo = 0.0f, hi = 0.0f;
        minMaxForRange(a, b, lo, hi);

        if (envelope != nullptr)
        {
            const float gain = envelope->gain * envelope->fadeGainAt(juce::jlimit<juce::int64>(0, envelope->length - 1, (a + b) / 2));
            lo *= gain;
            hi *= gain;
        }

        const float top = mid - juce::jlimit(0.0f, 1.0f, hi) * half;
        const float bot = mid - juce::jlimit(-1.0f, 0.0f, lo) * half;
        g.fillRect((float) x, top, 1.0f, juce::jmax(1.0f, bot - top));
    }
}

void TrackLanes::paintClip(juce::Graphics& g, const Clip& c, int track, bool selected, bool hovered)
{
    auto r = clipRect(c, track);
    const auto base = juce::Colour(engine.getTrack(track).colour);

    g.setColour(base.withAlpha(0.22f));
    g.fillRoundedRectangle(r, 4.0f);

    auto header = r.withHeight(14.0f);
    g.setColour(base.withAlpha(0.55f));
    g.fillRoundedRectangle(header, 4.0f);
    g.fillRect(header.withTop(header.getY() + 7.0f));

    if (r.getWidth() > 40.0f)
    {
        g.setColour(juce::Colours::black.withAlpha(0.75f));
        g.setFont(Theme::font(10.0f, true));
        const auto name = c.source->file.getFileNameWithoutExtension();
        g.drawText(name, header.reduced(5.0f, 0.0f).withX(juce::jmax(header.getX() + 5.0f, 2.0f)),
                   juce::Justification::centredLeft, true);
    }

    // Waveform
    auto waveArea = r.withTrimmedTop(15.0f).reduced(0.0f, 2.0f);
    const auto* src = c.source.get();
    paintWaveform(g, waveArea, c.start, [&](juce::int64 a, juce::int64 b, float& lo, float& hi)
    {
        const auto s0 = juce::jlimit<juce::int64>(0, src->getLength(), c.sourceOffset + a);
        const auto s1 = juce::jlimit<juce::int64>(0, src->getLength(), c.sourceOffset + b);
        if (s1 <= s0)
            return;

        if (s1 - s0 >= AudioSourceData::kPeakBlock && !src->peakMin.empty())
        {
            const auto blocks = (juce::int64) src->peakMin.size();
            const auto b0 = juce::jlimit<juce::int64>(0, blocks - 1, s0 / AudioSourceData::kPeakBlock);
            const auto b1 = juce::jlimit<juce::int64>(b0 + 1, blocks, s1 / AudioSourceData::kPeakBlock);
            for (auto i = b0; i < b1; ++i)
            {
                lo = juce::jmin(lo, src->peakMin[(size_t) i]);
                hi = juce::jmax(hi, src->peakMax[(size_t) i]);
            }
        }
        else
        {
            const float* l = src->audio.getReadPointer(0);
            const float* rr = src->audio.getReadPointer(1);
            for (auto i = s0; i < s1; ++i)
            {
                const float v = 0.5f * (l[i] + rr[i]);
                lo = juce::jmin(lo, v);
                hi = juce::jmax(hi, v);
            }
        }
    }, base.brighter(0.5f).withAlpha(0.9f), &c);

    // Fades: shade the attenuated area and draw the fade line
    auto drawFade = [&](float xFrom, float xTo, bool fadeIn)
    {
        if (std::abs(xTo - xFrom) < 1.0f)
            return;
        juce::Path shade;
        if (fadeIn)
            shade.addTriangle(xFrom, waveArea.getY(), xTo, waveArea.getY(), xFrom, waveArea.getBottom());
        else
            shade.addTriangle(xFrom, waveArea.getY(), xTo, waveArea.getY(), xTo, waveArea.getBottom());
        g.setColour(Theme::bg.withAlpha(0.45f));
        g.fillPath(shade);
        g.setColour(Theme::text.withAlpha(0.6f));
        if (fadeIn) g.drawLine(xFrom, waveArea.getBottom(), xTo, waveArea.getY(), 1.0f);
        else        g.drawLine(xFrom, waveArea.getY(), xTo, waveArea.getBottom(), 1.0f);
    };
    const float fadeInX = view.xFor(c.start + c.fadeIn);
    const float fadeOutX = view.xFor(c.end() - c.fadeOut);
    drawFade(r.getX(), fadeInX, true);
    drawFade(fadeOutX, r.getRight(), false);

    if ((hovered || selected) && r.getWidth() > 24.0f)
    {
        g.setColour(Theme::text);
        g.fillRect(juce::Rectangle<float>(6.0f, 6.0f).withCentre({ juce::jmax(fadeInX, r.getX() + 3.0f), r.getY() + 3.0f }));
        g.fillRect(juce::Rectangle<float>(6.0f, 6.0f).withCentre({ juce::jmin(fadeOutX, r.getRight() - 3.0f), r.getY() + 3.0f }));
    }

    g.setColour(selected ? Theme::text : base.withAlpha(0.7f));
    g.drawRoundedRectangle(r, 4.0f, selected ? 1.6f : 1.0f);
}

void TrackLanes::paintLiveTakes(juce::Graphics& g)
{
    const auto* session = engine.getRecordSession();
    if (session == nullptr)
        return;

    const auto first = session->firstCapture.load();
    if (first == std::numeric_limits<juce::int64>::min())
        return;
    const auto start = first - session->latency;

    for (auto& take : session->takes)
    {
        const auto written = take->written.load(std::memory_order_acquire);
        if (written <= 0)
            continue;

        Clip shape;
        shape.start = start;
        shape.length = written;
        auto r = clipRect(shape, take->track);

        g.setColour(Theme::record.withAlpha(0.25f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(Theme::record.withAlpha(0.8f));
        g.fillRoundedRectangle(r.withHeight(14.0f), 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(Theme::font(10.0f, true));
        g.drawText("Recording", r.withHeight(14.0f).reduced(5.0f, 0.0f), juce::Justification::centredLeft, true);

        const auto blocks = (juce::int64) (written / AudioSourceData::kPeakBlock);
        paintWaveform(g, r.withTrimmedTop(15.0f).reduced(0.0f, 2.0f), start,
                      [&](juce::int64 a, juce::int64 b, float& lo, float& hi)
        {
            const auto b0 = juce::jlimit<juce::int64>(0, blocks, a / AudioSourceData::kPeakBlock);
            const auto b1 = juce::jlimit<juce::int64>(b0, blocks, juce::jmax(b0 + 1, b / AudioSourceData::kPeakBlock));
            for (auto i = b0; i < b1; ++i)
            {
                lo = juce::jmin(lo, take->peakMin[(size_t) i]);
                hi = juce::jmax(hi, take->peakMax[(size_t) i]);
            }
        }, Theme::record.brighter(0.6f), nullptr);

        g.setColour(Theme::record);
        g.drawRoundedRectangle(r, 4.0f, 1.2f);
    }
}

//==============================================================================
void TrackLanes::mouseMove(const juce::MouseEvent& e)
{
    const auto hit = findHit(e.position);
    if (hit.clipId != hover.clipId || hit.zone != hover.zone)
    {
        hover = hit;
        repaint();
    }

    switch (hit.zone)
    {
        case Zone::TrimStart:
        case Zone::TrimEnd:  setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); break;
        case Zone::FadeIn:
        case Zone::FadeOut:  setMouseCursor(juce::MouseCursor::CrosshairCursor); break;
        case Zone::Body:     setMouseCursor(juce::MouseCursor::DraggingHandCursor); break;
        case Zone::None:     setMouseCursor(juce::MouseCursor::NormalCursor); break;
    }
}

void TrackLanes::mouseExit(const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void TrackLanes::mouseDown(const juce::MouseEvent& e)
{
    dragHit = findHit(e.position);
    dragActive = false;
    dragDownSample = view.sampleAt(e.position.x);
    dragDownTrack = trackAtY(e.position.y);

    if (onTrackClicked)
        onTrackClicked(dragDownTrack);

    if (dragHit.clipId == 0)
    {
        if (!e.mods.isShiftDown())
            selection.clear();
        if (!e.mods.isPopupMenu())
            engine.setPosition(juce::jmax<juce::int64>(0, snapped(dragDownSample, e.mods)));
        repaint();
        if (onViewChanged) onViewChanged();
        return;
    }

    if (e.mods.isShiftDown())
    {
        if (selection.count(dragHit.clipId)) selection.erase(dragHit.clipId);
        else                                 selection.insert(dragHit.clipId);
    }
    else if (selection.count(dragHit.clipId) == 0)
    {
        selection = { dragHit.clipId };
    }
    repaint();

    if (e.mods.isPopupMenu())
    {
        selection = { dragHit.clipId };
        showClipMenu(dragHit.clipId);
        dragHit = {};
    }
}

std::function<bool(Arrangement&)> TrackLanes::editForDrag(const juce::MouseEvent& e) const
{
    const auto& arr = project.getArrangement();
    const Clip* found = arr.findClip(dragHit.clipId);
    if (found == nullptr)
        return {};

    const Clip orig = *found;
    const auto id = orig.id;
    const auto mouse = view.sampleAt(e.position.x);
    const auto delta = mouse - dragDownSample;

    switch (dragHit.zone)
    {
        case Zone::Body:
        {
            const auto shift = juce::jmax<juce::int64>(0, snapped(orig.start + delta, e.mods)) - orig.start;
            const int trackShift = trackAtY(e.position.y) - dragDownTrack;
            auto ids = selection;
            ids.insert(id);

            return [ids, shift, trackShift](Arrangement& a)
            {
                std::vector<std::pair<int, Clip>> moving;
                int minTrack = kNumTracks, maxTrack = -1;
                juce::int64 allowedShift = shift;
                for (auto clipId : ids)
                {
                    int t = -1;
                    if (auto* m = a.findClip(clipId, &t))
                    {
                        moving.push_back({ t, *m });
                        minTrack = juce::jmin(minTrack, t);
                        maxTrack = juce::jmax(maxTrack, t);
                        allowedShift = juce::jmax(allowedShift, -m->start);   // nothing before zero
                    }
                }
                const int ts = juce::jlimit(-minTrack, kNumTracks - 1 - maxTrack, trackShift);
                if (moving.empty() || (allowedShift == 0 && ts == 0))
                    return false;

                for (auto& m : moving)
                    a.removeClip(m.second.id);
                for (auto& m : moving)
                {
                    m.second.start += allowedShift;
                    a.insertClip(m.first + ts, m.second);
                }
                return true;
            };
        }
        case Zone::TrimStart:
        {
            const auto newStart = snapped(orig.start + delta, e.mods);
            return [id, newStart](Arrangement& a) { return a.trimStart(id, newStart); };
        }
        case Zone::TrimEnd:
        {
            const auto newEnd = snapped(orig.end() + delta, e.mods);
            return [id, newEnd](Arrangement& a) { return a.trimEnd(id, newEnd); };
        }
        case Zone::FadeIn:
        {
            const auto fade = juce::jmax<juce::int64>(0, mouse - orig.start);
            const auto out = orig.fadeOut;
            return [id, fade, out](Arrangement& a) { return a.setFades(id, fade, out); };
        }
        case Zone::FadeOut:
        {
            const auto fade = juce::jmax<juce::int64>(0, orig.end() - mouse);
            const auto in = orig.fadeIn;
            return [id, fade, in](Arrangement& a) { return a.setFades(id, in, fade); };
        }
        case Zone::None:
            break;
    }
    return {};
}

void TrackLanes::mouseDrag(const juce::MouseEvent& e)
{
    if (dragHit.clipId == 0)
    {
        // Dragging on empty space scrubs the playhead.
        engine.setPosition(juce::jmax<juce::int64>(0, snapped(view.sampleAt(e.position.x), e.mods)));
        repaint();
        if (onViewChanged) onViewChanged();
        return;
    }

    if (!dragActive && e.getDistanceFromDragStart() < 3)
        return;
    dragActive = true;

    preview = project.getArrangement();
    if (auto edit = editForDrag(e))
        edit(*preview);
    repaint();
}

void TrackLanes::mouseUp(const juce::MouseEvent& e)
{
    if (dragActive && dragHit.clipId != 0)
    {
        if (auto edit = editForDrag(e))
            project.perform(edit);
    }
    dragActive = false;
    dragHit = {};
    preview.reset();
    pruneSelection();
    repaint();
}

void TrackLanes::showClipMenu(juce::int64 clipId)
{
    const auto* c = project.getArrangement().findClip(clipId);
    if (c == nullptr)
        return;

    const auto playhead = engine.getPosition();
    const bool canSplit = playhead > c->start && playhead < c->end();

    juce::PopupMenu menu;
    menu.addItem(1, "Split at Playhead", canSplit);
    menu.addItem(2, "Delete");
    menu.addItem(3, "Reset Fades");
    menu.addSeparator();
    menu.addItem(4, "Gain +3 dB");
    menu.addItem(5, "Gain -3 dB");
    menu.addItem(6, "Reset Gain", std::abs(c->gain - 1.0f) > 0.0001f);

    juce::Component::SafePointer<TrackLanes> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe, clipId, playhead](int result)
    {
        if (safe == nullptr || result == 0)
            return;
        auto& proj = safe->project;
        const auto* clip = proj.getArrangement().findClip(clipId);
        if (clip == nullptr)
            return;
        const float gain = clip->gain;

        switch (result)
        {
            case 1: proj.perform([=](Arrangement& a) { return a.splitClip(clipId, playhead); }); break;
            case 2: proj.perform([=](Arrangement& a) { return a.removeClip(clipId); }); break;
            case 3: proj.perform([=](Arrangement& a) { return a.setFades(clipId, Arrangement::kEdgeFade, Arrangement::kEdgeFade); }); break;
            case 4: proj.perform([=](Arrangement& a) { return a.setGain(clipId, gain * 1.4125f); }); break;
            case 5: proj.perform([=](Arrangement& a) { return a.setGain(clipId, gain / 1.4125f); }); break;
            case 6: proj.perform([=](Arrangement& a) { return a.setGain(clipId, 1.0f); }); break;
            default: break;
        }
        safe->pruneSelection();
        safe->repaint();
    });
}

void TrackLanes::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        view.zoomAround(e.position.x, std::pow(0.85, wheel.deltaY * 6.0));
    }
    else
    {
        double dx = wheel.deltaX;
        double dy = wheel.deltaY;
        if (e.mods.isShiftDown() && std::abs(dx) < 0.0001)
        {
            dx = dy;
            dy = 0.0;
        }
        view.viewStart = juce::jmax(0.0, view.viewStart - dx * 400.0 * view.samplesPerPixel);
        scrollY -= (int) std::round(dy * 300.0);
    }
    if (onViewChanged) onViewChanged();
}

void TrackLanes::mouseMagnify(const juce::MouseEvent& e, float scale)
{
    view.zoomAround(e.position.x, 1.0 / juce::jmax(0.1f, scale));
    if (onViewChanged) onViewChanged();
}

bool TrackLanes::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File(f).hasFileExtension("wav;aif;aiff;flac;ogg;mp3"))
            return true;
    return false;
}

void TrackLanes::filesDropped(const juce::StringArray& files, int x, int y)
{
    int track = trackAtY((float) y);
    const auto position = juce::jmax<juce::int64>(0, snapped(view.sampleAt((float) x), {}));
    juce::StringArray errors;

    for (auto& path : files)
    {
        juce::String error;
        if (!project.importAudio(juce::File(path), track, position, error))
            errors.add(error);
        track = juce::jmin(kNumTracks - 1, track + 1);   // several files go on consecutive tracks
    }

    if (!errors.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import", errors.joinIntoString("\n"));
}
