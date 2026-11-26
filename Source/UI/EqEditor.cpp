#include "EqEditor.h"
#include "Theme.h"

namespace
{
    constexpr float kLowHz = EqEffect::kMinHz, kHighHz = EqEffect::kMaxHz;
    constexpr float kRangeDb = EqEffect::kMaxGainDb;              // the curve's scale: +-18 dB
    constexpr float kSpectrumTop = 0.0f, kSpectrumFloor = -90.0f;  // the analyser's scale
    constexpr int kFftOrder = 12, kFftSize = 1 << kFftOrder;
    static_assert(kFftSize <= SpectrumTap::kSize);

    juce::String hzText(double hz)
    {
        return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz";
    }
}

//==============================================================================
class EqEditor::Graph : public juce::Component
{
public:
    Graph(EqEditor& o, EqEffect& e) : owner(o), eq(e), fft(kFftOrder), window((size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann, false)
    {
        setRepaintsOnMouseActivity(false);
    }

    // --- mapping
    float xForHz(double hz) const { return (float) (std::log(hz / kLowHz) / std::log(kHighHz / kLowHz)) * (float) getWidth(); }
    double hzForX(float x) const { return kLowHz * std::pow(kHighHz / kLowHz, juce::jlimit(0.0, 1.0, (double) x / juce::jmax(1, getWidth()))); }
    float yForDb(double db) const { return (float) (0.5 - db / (2.0 * kRangeDb)) * (float) getHeight(); }
    double dbForY(float y) const { return (0.5 - (double) y / juce::jmax(1, getHeight())) * 2.0 * kRangeDb; }
    float yForSpectrum(float db) const { return (kSpectrumTop - db) / (kSpectrumTop - kSpectrumFloor) * (float) getHeight(); }

    juce::Point<float> nodeFor(int band) const
    {
        const auto& b = eq.bands[(size_t) band];
        const double gain = EqEffect::hasGain(b.getType()) ? b.gain->getCurrentValue() : 0.0;
        return { xForHz(b.freq->getCurrentValue()), yForDb(gain) };
    }

    /** Takes in the latest audio. Call from the timer. */
    void analyse()
    {
        update(eq.before, before);
        update(eq.after, after);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour(Theme::bg);
        g.fillRoundedRectangle(area, 4.0f);
        g.saveState();
        g.reduceClipRegion(getLocalBounds());

        paintGrid(g);
        paintSpectrum(g, before, Theme::textDim.withAlpha(0.35f), false);
        paintSpectrum(g, after, Theme::accent.withAlpha(0.22f), true);

        // The selected band on its own, then the whole curve
        const int sel = owner.getSelected();
        if (eq.bands[(size_t) sel].isOn())
        {
            auto p = curve([&](double hz) { return eq.bandResponseDb(sel, hz); });
            auto fill = p;
            fill.lineTo((float) getWidth(), yForDb(0));
            fill.lineTo(0, yForDb(0));
            fill.closeSubPath();
            g.setColour(bandColour(sel).withAlpha(0.18f));
            g.fillPath(fill);
        }
        g.setColour(Theme::text);
        g.strokePath(curve([&](double hz) { return eq.responseDb(hz); }), juce::PathStrokeType(2.0f));

        for (int i = 0; i < EqEffect::kBands; ++i)
            paintNode(g, i);

        paintReadout(g);
        g.restoreState();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        hovered = hit(e.position);
        mouse = e.position;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hovered = -1;
        mouse = {};
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = hit(e.position);
        if (dragging < 0)
            return;
        owner.select(dragging);
        auto& b = eq.bands[(size_t) dragging];
        eq.edit.getUndoManager().beginNewTransaction("Change EQ Band " + juce::String(dragging + 1));
        b.freq->parameterChangeGestureBegin();
        b.gain->parameterChangeGestureBegin();
        grabOffset = nodeFor(dragging) - e.position;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        auto& b = eq.bands[(size_t) dragging];
        const auto p = e.position + grabOffset;
        b.freq->setParameter((float) juce::jlimit((double) kLowHz, (double) kHighHz, hzForX(p.x)), juce::sendNotification);
        if (EqEffect::hasGain(b.getType()))
            b.gain->setParameter((float) juce::jlimit(-(double) kRangeDb, (double) kRangeDb, dbForY(p.y)), juce::sendNotification);
        mouse = e.position;
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging < 0)
            return;
        auto& b = eq.bands[(size_t) dragging];
        b.freq->parameterChangeGestureEnd();
        b.gain->parameterChangeGestureEnd();
        dragging = -1;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int band = hit(e.position);
        if (band < 0)
            return;
        auto* on = eq.bands[(size_t) band].on;
        eq.edit.getUndoManager().beginNewTransaction("Switch EQ Band " + juce::String(band + 1));
        on->parameterChangeGestureBegin();
        on->setParameter(on->getCurrentValue() >= 0.5f ? 0.0f : 1.0f, juce::sendNotification);
        on->parameterChangeGestureEnd();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int band = hit(e.position) >= 0 ? hit(e.position) : owner.getSelected();
        auto* q = eq.bands[(size_t) band].q;
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastWheel > 600)   // one undo step per spin of the wheel
            eq.edit.getUndoManager().beginNewTransaction("Change EQ Band " + juce::String(band + 1) + " Q");
        lastWheel = now;
        const float factor = std::pow(1.25f, (w.isReversed ? -w.deltaY : w.deltaY) * 4.0f);
        q->setParameter(juce::jlimit(q->valueRange.start, q->valueRange.end, q->getCurrentValue() * factor), juce::sendNotification);
        repaint();
    }

private:
    struct Spectrum
    {
        std::vector<float> db;   // one value per point across the graph, smoothed
    };

    static constexpr int kStep = 2;   // pixels between the analyser's points

    void update(const SpectrumTap& tap, Spectrum& s)
    {
        std::fill(data.begin(), data.end(), 0.0f);
        tap.read(data.data(), kFftSize);
        window.multiplyWithWindowingTable(data.data(), (size_t) kFftSize);
        fft.performFrequencyOnlyForwardTransform(data.data(), true);

        const int points = getWidth() / kStep + 2;
        s.db.resize((size_t) points, kSpectrumFloor);
        const double binHz = eq.rate() / kFftSize;
        const float scale = 4.0f / kFftSize;   // a full-scale sine reads 0 dB through the Hann window
        for (int i = 0; i < points; ++i)
        {
            // The loudest bin under this point (and, below a bin's width, the one nearest)
            const double lo = hzForX((float) (i * kStep) - kStep * 0.5f), hi = hzForX((float) (i * kStep) + kStep * 0.5f);
            int from = (int) std::floor(lo / binHz), to = (int) std::ceil(hi / binHz);
            from = juce::jlimit(1, kFftSize / 2 - 1, from);
            to = juce::jlimit(from, kFftSize / 2 - 1, to);
            float mag = 0.0f;
            for (int k = from; k <= to; ++k)
                mag = juce::jmax(mag, data[(size_t) k]);
            const float db = juce::jmax(kSpectrumFloor, juce::Decibels::gainToDecibels(mag * scale, kSpectrumFloor));
            auto& v = s.db[(size_t) i];
            v = db > v ? db : v + (db - v) * 0.18f;   // jumps up, falls slowly
        }
    }

    void paintGrid(juce::Graphics& g)
    {
        g.setFont(Theme::font(9.0f));
        for (double hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            const float x = xForHz(hz);
            g.setColour(Theme::gridBeat);
            g.drawVerticalLine(juce::roundToInt(x), 0.0f, (float) getHeight());
            if (hz > 20.0 && hz < 20000.0)
            {
                g.setColour(Theme::textDim.withAlpha(0.7f));
                const auto label = hz >= 1000.0 ? juce::String((int) (hz / 1000.0)) + "k" : juce::String((int) hz);
                g.drawText(label, juce::Rectangle<float>(x + 3.0f, (float) getHeight() - 14.0f, 30.0f, 12.0f), juce::Justification::left, false);
            }
        }
        for (int db = -12; db <= 12; db += 6)
        {
            const float y = yForDb(db);
            g.setColour(db == 0 ? Theme::gridBar : Theme::gridBeat);
            g.drawHorizontalLine(juce::roundToInt(y), 0.0f, (float) getWidth());
            g.setColour(Theme::textDim.withAlpha(0.7f));
            g.drawText((db > 0 ? "+" : "") + juce::String(db), juce::Rectangle<float>((float) getWidth() - 28.0f, y - 12.0f, 24.0f, 11.0f),
                       juce::Justification::right, false);
        }
    }

    void paintSpectrum(juce::Graphics& g, const Spectrum& s, juce::Colour colour, bool filled)
    {
        if (s.db.empty())
            return;
        juce::Path p;
        for (size_t i = 0; i < s.db.size(); ++i)
        {
            const float x = (float) ((int) i * kStep), y = yForSpectrum(s.db[i]);
            if (i == 0) p.startNewSubPath(x, y);
            else        p.lineTo(x, y);
        }
        if (filled)
        {
            p.lineTo((float) getWidth(), (float) getHeight());
            p.lineTo(0.0f, (float) getHeight());
            p.closeSubPath();
            g.setColour(colour);
            g.fillPath(p);
        }
        else
        {
            g.setColour(colour);
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
    }

    template <typename Response>
    juce::Path curve(Response&& response) const
    {
        juce::Path p;
        for (int x = 0; x <= getWidth(); x += 2)
        {
            const float y = juce::jlimit(-4.0f, (float) getHeight() + 4.0f, yForDb(response(hzForX((float) x))));
            if (x == 0) p.startNewSubPath((float) x, y);
            else        p.lineTo((float) x, y);
        }
        return p;
    }

    void paintNode(juce::Graphics& g, int band)
    {
        const auto& b = eq.bands[(size_t) band];
        const auto c = nodeFor(band);
        const bool on = b.isOn(), chosen = band == owner.getSelected();
        const float radius = chosen || band == hovered ? 9.0f : 8.0f;
        const auto box = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(c);
        const auto colour = bandColour(band);
        g.setColour(on ? colour : Theme::panel);
        g.fillEllipse(box);
        g.setColour(chosen ? Theme::text : on ? colour.darker(0.4f) : colour.withAlpha(0.6f));
        g.drawEllipse(box, chosen ? 2.0f : 1.5f);
        g.setColour(on ? Theme::bg : colour.withAlpha(0.8f));
        g.setFont(Theme::font(10.0f, true));
        g.drawText(juce::String(band + 1), box, juce::Justification::centred, false);
    }

    void paintReadout(juce::Graphics& g)
    {
        // What's under the dot being dragged or hovered over, otherwise where the mouse points
        const int band = dragging >= 0 ? dragging : hovered;
        juce::String text;
        if (band >= 0)
        {
            const auto& b = eq.bands[(size_t) band];
            text = juce::String(band + 1) + "  " + EqEffect::typeNames()[(int) b.getType()] + "  " + hzText(b.freq->getCurrentValue());
            if (EqEffect::hasGain(b.getType()))
                text << "  " << b.gain->getCurrentValueAsString();
            text << "  Q " << juce::String(b.q->getCurrentValue(), 2);
            if (!b.isOn())
                text << "  (off)";
        }
        else if (getLocalBounds().toFloat().contains(mouse) && !mouse.isOrigin())
        {
            text = hzText(hzForX(mouse.x));
        }
        if (text.isEmpty())
            return;
        g.setFont(Theme::font(11.0f, true));
        g.setColour(Theme::text);
        g.drawText(text, juce::Rectangle<int>(8, 6, getWidth() - 16, 14), juce::Justification::left, false);
    }

    int hit(juce::Point<float> p) const
    {
        int best = -1;
        float nearest = 12.0f;
        for (int i = EqEffect::kBands; --i >= 0;)   // the top one wins
        {
            const float d = nodeFor(i).getDistanceFrom(p);
            if (d < nearest)
                nearest = d, best = i;
        }
        return best;
    }

    EqEditor& owner;
    EqEffect& eq;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> data = std::vector<float>((size_t) kFftSize * 2);
    Spectrum before, after;
    int hovered = -1, dragging = -1;
    juce::Point<float> grabOffset, mouse;
    juce::uint32 lastWheel = 0;
};

//==============================================================================
EqEditor::EqEditor(EqEffect& e)
    : eq(e), graph(std::make_unique<Graph>(*this, e)),
      outputControls(e, [](const juce::String& row) { return row == "Output"; })
{
    eq.before.active = true;
    eq.after.active = true;
    addAndMakeVisible(*graph);
    for (int i = 0; i < EqEffect::kBands; ++i)
    {
        auto* b = bandButtons.add(new juce::TextButton(juce::String(i + 1)));
        b->setColour(juce::TextButton::buttonOnColourId, bandColour(i));
        b->setColour(juce::TextButton::textColourOnId, Theme::bg);
        b->setTooltip("Show band " + juce::String(i + 1) + "'s controls");
        b->setWantsKeyboardFocus(false);
        b->onClick = [this, i] { select(i); };
        addAndMakeVisible(b);
    }
    addAndMakeVisible(outputControls);
    select(selected);
    startTimerHz(30);
}

EqEditor::~EqEditor()
{
    stopTimer();
    eq.before.active = false;
    eq.after.active = false;
}

juce::Colour EqEditor::bandColour(int band)
{
    static const juce::Colour colours[] { juce::Colour(0xffe5484d), juce::Colour(0xfff2b84b), juce::Colour(0xff34c79a),
                                          juce::Colour(0xff4f9dff), juce::Colour(0xffb57cf2) };
    return colours[juce::jlimit(0, EqEffect::kBands - 1, band)];
}

void EqEditor::select(int band)
{
    selected = juce::jlimit(0, EqEffect::kBands - 1, band);
    if (bandControls == nullptr || bandControls->getComponentID() != EqEffect::rowFor(selected))
    {
        const auto row = EqEffect::rowFor(selected);
        bandControls = std::make_unique<EffectControls>(eq, [row](const juce::String& r) { return r == row; });
        bandControls->setComponentID(row);
        addAndMakeVisible(*bandControls);
    }
    for (int i = 0; i < bandButtons.size(); ++i)
        bandButtons[i]->setToggleState(i == selected, juce::dontSendNotification);
    layout();
    graph->repaint();
}

void EqEditor::layout()
{
    const int width = juce::jmax(bandControls->naturalWidth() + outputControls.naturalWidth(), 640);
    const int height = kMargin + kGraphHeight + 10 + kButtons + 6
                       + juce::jmax(bandControls->heightForWidth(bandControls->naturalWidth()),
                                    outputControls.heightForWidth(outputControls.naturalWidth()))
                       + kMargin;
    if (getWidth() != width + 2 * kMargin || getHeight() != height)
        setSize(width + 2 * kMargin, height);
    else
        resized();
}

void EqEditor::resized()
{
    if (bandControls == nullptr)
        return;
    auto r = getLocalBounds().reduced(kMargin);
    graph->setBounds(r.removeFromTop(kGraphHeight));
    r.removeFromTop(10);
    auto buttons = r.removeFromTop(kButtons);
    for (auto* b : bandButtons)
    {
        b->setBounds(buttons.removeFromLeft(40));
        buttons.removeFromLeft(4);
    }
    r.removeFromTop(6);
    outputControls.setBounds(r.removeFromRight(outputControls.naturalWidth()));
    bandControls->setBounds(r.withWidth(bandControls->naturalWidth()));
}

void EqEditor::paint(juce::Graphics& g) { g.fillAll(Theme::panel); }

void EqEditor::timerCallback()
{
    for (int i = 0; i < bandButtons.size(); ++i)   // dim the buttons of bands that are off
    {
        const float alpha = eq.bands[(size_t) i].isOn() ? 1.0f : 0.45f;
        if (!juce::approximatelyEqual(bandButtons[i]->getAlpha(), alpha))
            bandButtons[i]->setAlpha(alpha);
    }
    graph->analyse();
}
