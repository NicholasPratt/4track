#pragma once

#include "Theme.h"

/**
    Peak meter with a dB scale (-60 .. +6 dBFS), time-based fall-off, peak hold and a
    clip indicator (click to clear). Feed it linear peaks from a UI timer with setPeaks().
*/
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter(bool isVertical = true, bool showScale = false)
        : vertical(isVertical), scale(showScale) {}

    void setPeaks(float left, float right)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = lastTime > 0.0 ? (float) ((now - lastTime) / 1000.0) : 0.0f;
        lastTime = now;

        const float in[2] = { toDb(left), toDb(right) };
        for (int ch = 0; ch < 2; ++ch)
        {
            level[ch] = juce::jmax(in[ch], level[ch] - kFallDbPerSecond * dt);
            if (in[ch] >= hold[ch]) { hold[ch] = in[ch]; holdTime[ch] = now; }
            else if (now - holdTime[ch] > 1500.0) hold[ch] = juce::jmax(level[ch], hold[ch] - kFallDbPerSecond * dt);
            if (in[ch] >= 0.0f) clipped = true;
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        juce::Rectangle<float> scaleArea;
        if (scale && vertical)
            scaleArea = area.removeFromRight(18.0f);

        g.setColour(Theme::bg);
        g.fillRoundedRectangle(area, 2.0f);

        const float gap = 1.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = vertical
                ? area.withWidth((area.getWidth() - gap) / 2).withX(area.getX() + (float) ch * (area.getWidth() + gap) / 2)
                : area.withHeight((area.getHeight() - gap) / 2).withY(area.getY() + (float) ch * (area.getHeight() + gap) / 2);
            drawBar(g, bar, level[ch], hold[ch]);
        }

        if (clipped)
        {
            g.setColour(Theme::record);
            if (vertical) g.fillRect(area.withHeight(3.0f));
            else          g.fillRect(area.withX(area.getRight() - 3.0f).withWidth(3.0f));
        }

        if (scale && vertical)
        {
            g.setFont(Theme::font(9.0f));
            g.setColour(Theme::textDim);
            for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            {
                const float y = area.getBottom() - proportion(db) * area.getHeight();
                g.drawText(juce::String((int) db), scaleArea.withY(y - 5.0f).withHeight(10.0f),
                           juce::Justification::centredRight, false);
            }
        }
    }

    void mouseDown(const juce::MouseEvent&) override { clipped = false; repaint(); }

private:
    static constexpr float kMinDb = -60.0f, kMaxDb = 6.0f, kFallDbPerSecond = 26.0f;

    static float toDb(float linear) { return linear <= 0.000001f ? kMinDb : juce::jlimit(kMinDb, kMaxDb, 20.0f * std::log10(linear)); }

    // Expands the top of the range so -12..0 dB gets about half the meter.
    static float proportion(float db) { return std::pow((db - kMinDb) / (kMaxDb - kMinDb), 2.0f); }

    void drawBar(juce::Graphics& g, juce::Rectangle<float> bar, float db, float holdDb) const
    {
        const float p = proportion(db);
        if (p > 0.0f)
        {
            juce::ColourGradient grad(Theme::monitor, vertical ? bar.getX() : bar.getX(), vertical ? bar.getBottom() : bar.getY(),
                                      Theme::record, vertical ? bar.getX() : bar.getRight(), vertical ? bar.getY() : bar.getY(), false);
            grad.addColour(proportion(-12.0f), Theme::monitor);
            grad.addColour(proportion(-3.0f), Theme::mute);
            g.setGradientFill(grad);
            if (vertical) g.fillRect(bar.withTop(bar.getBottom() - p * bar.getHeight()));
            else          g.fillRect(bar.withWidth(p * bar.getWidth()));
        }

        const float hp = proportion(holdDb);
        if (hp > 0.01f)
        {
            g.setColour(holdDb >= 0.0f ? Theme::record : Theme::text.withAlpha(0.8f));
            if (vertical) g.fillRect(bar.getX(), bar.getBottom() - hp * bar.getHeight(), bar.getWidth(), 1.5f);
            else          g.fillRect(bar.getX() + hp * bar.getWidth() - 1.5f, bar.getY(), 1.5f, bar.getHeight());
        }
    }

    bool vertical, scale;
    float level[2] = { kMinDb, kMinDb }, hold[2] = { kMinDb, kMinDb };
    double holdTime[2] = {}, lastTime = 0.0;
    bool clipped = false;
};

/** Transport-style button drawn with vector icons (no reliance on font glyphs). */
class IconButton : public juce::Button
{
public:
    enum class Icon { ToStart, Play, Stop, Record, Metronome, CountIn, Tuner };

    IconButton(const juce::String& name, Icon i, juce::Colour onColour = Theme::accent)
        : juce::Button(name), icon(i), activeColour(onColour) {}

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        const bool on = getToggleState();
        auto bg = on ? activeColour.withAlpha(0.25f) : Theme::control;
        if (down) bg = bg.brighter(0.15f);
        else if (highlighted) bg = bg.brighter(0.08f);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 5.0f);

        auto c = on ? activeColour : (icon == Icon::Record ? Theme::record.withAlpha(0.85f) : Theme::text);
        g.setColour(c);
        auto r = bounds.withSizeKeepingCentre(14.0f, 14.0f);

        juce::Path p;
        switch (icon)
        {
            case Icon::Play:
                p.addTriangle(r.getX() + 2, r.getY(), r.getX() + 2, r.getBottom(), r.getRight(), r.getCentreY());
                break;
            case Icon::Stop:
                p.addRoundedRectangle(r.reduced(1.5f), 1.5f);
                break;
            case Icon::Record:
                p.addEllipse(r.reduced(1.0f));
                break;
            case Icon::Metronome:
            {
                const float x = r.getCentreX(), y = r.getY();
                p.startNewSubPath(x - 2, y + 1);
                p.lineTo(x + 2, y + 1);
                p.lineTo(x + 6, y + 13);
                p.lineTo(x - 6, y + 13);
                p.closeSubPath();
                g.strokePath(p, juce::PathStrokeType(1.25f));
                g.drawLine(x, y + 10, x + 6, y + 2, 1.4f);
                g.fillEllipse(x + 5, y + 1, 2.5f, 2.5f);
                return;
            }
            case Icon::CountIn:
                for (int i = 0; i < 4; ++i)
                    p.addEllipse(r.getX() + (float) i * 3.5f, r.getCentreY() - 1.5f, 2.5f, 2.5f);
                p.addRectangle(r.getX(), r.getY() + 2, r.getWidth(), 1.2f);
                p.addRectangle(r.getX(), r.getBottom() - 3, r.getWidth(), 1.2f);
                break;
            case Icon::Tuner:
            {
                const auto x = r.getCentreX(), y = r.getY();
                p.startNewSubPath(x - 4, y);
                p.lineTo(x - 4, y + 5);
                p.quadraticTo(x - 4, y + 9, x, y + 9);
                p.quadraticTo(x + 4, y + 9, x + 4, y + 5);
                p.lineTo(x + 4, y);
                g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
                g.drawLine(x, y + 9, x, y + 14, 1.6f);
                return;
            }
            case Icon::ToStart:
                p.addRectangle(r.getX(), r.getY(), 2.5f, r.getHeight());
                p.addTriangle(r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + 2.5f, r.getCentreY());
                break;
        }
        g.fillPath(p);
    }

private:
    Icon icon;
    juce::Colour activeColour;
};

/** Small toggle with a text label, coloured when on. */
class ToggleChip : public juce::TextButton
{
public:
    ToggleChip(const juce::String& text, juce::Colour onColour, const juce::String& tooltip = {})
    {
        setButtonText(text);
        setClickingTogglesState(true);
        setColour(juce::TextButton::buttonOnColourId, onColour);
        setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        setTooltip(tooltip);
    }
};
