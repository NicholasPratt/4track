#include "Ruler.h"

Ruler::Ruler(AudioEngine& e, Project& p, TimelineView& v) : engine(e), project(p), view(v) {}

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
    const float barArea = height - kPunchStripHeight;

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
        const bool on = engine.punchOn.load();
        auto r = strip.withX(view.xFor(pin)).withRight(view.xFor(pout)).reduced(0.0f, 2.0f);
        g.setColour(Theme::record.withAlpha(on ? 0.85f : 0.3f));
        g.fillRoundedRectangle(r, 2.0f);
        if (r.getWidth() > 50.0f)
        {
            g.setColour(on ? juce::Colours::white : Theme::text.withAlpha(0.6f));
            g.setFont(Theme::font(9.5f, true));
            g.drawText(on ? "PUNCH" : "PUNCH (OFF)", r, juce::Justification::centred, false);
        }
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
    const auto s = snapped(view.sampleAt(e.position.x), e.mods);
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
            engine.punchOn = true;
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

void Ruler::mouseUp(const juce::MouseEvent&)
{
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
        engine.punchOn = !engine.punchOn.load();
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
