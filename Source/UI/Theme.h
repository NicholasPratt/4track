#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Theme
{
    inline const juce::Colour bg         { 0xff15171b };
    inline const juce::Colour panel      { 0xff1e2126 };
    inline const juce::Colour panelLight { 0xff292d34 };
    inline const juce::Colour control    { 0xff30353d };
    inline const juce::Colour outline    { 0xff3a4049 };
    inline const juce::Colour laneA      { 0xff1a1d21 };
    inline const juce::Colour laneB      { 0xff1d2025 };
    inline const juce::Colour gridBeat   { 0xff262a30 };
    inline const juce::Colour gridBar    { 0xff363b44 };
    inline const juce::Colour text       { 0xffdde1e7 };
    inline const juce::Colour textDim    { 0xff8b929d };
    inline const juce::Colour accent     { 0xff4f9dff };
    inline const juce::Colour record     { 0xffe5484d };
    inline const juce::Colour mute       { 0xfff2b84b };
    inline const juce::Colour solo       { 0xff4fd1e0 };
    inline const juce::Colour monitor    { 0xff34c79a };
    inline const juce::Colour playhead   { 0xfff5f7fa };

    inline juce::Font font(float size, bool bold = false)
    {
        return juce::Font(size, bold ? juce::Font::bold : juce::Font::plain);
    }
}

class DawLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DawLookAndFeel();

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background,
                              bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos,
                          float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override { return Theme::font(12.0f); }
    juce::Font getPopupMenuFont() override { return Theme::font(13.0f); }
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
};
