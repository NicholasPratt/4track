#include "Theme.h"

DawLookAndFeel::DawLookAndFeel()
{
    setColourScheme({ Theme::panel, Theme::bg, Theme::panelLight, Theme::outline, Theme::text,
                      Theme::accent, juce::Colours::white, Theme::panelLight, Theme::text });

    setColour(juce::ResizableWindow::backgroundColourId, Theme::bg);
    setColour(juce::TextButton::buttonColourId, Theme::control);
    setColour(juce::TextButton::buttonOnColourId, Theme::accent);
    setColour(juce::TextButton::textColourOffId, Theme::text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ComboBox::backgroundColourId, Theme::control);
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::textColourId, Theme::text);
    setColour(juce::ComboBox::arrowColourId, Theme::textDim);
    setColour(juce::PopupMenu::backgroundColourId, Theme::panelLight);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, Theme::accent);
    setColour(juce::PopupMenu::textColourId, Theme::text);
    setColour(juce::Label::textColourId, Theme::text);
    setColour(juce::Label::textWhenEditingColourId, Theme::text);
    setColour(juce::Label::backgroundWhenEditingColourId, Theme::bg);
    setColour(juce::Label::outlineWhenEditingColourId, Theme::accent);
    setColour(juce::TextEditor::backgroundColourId, Theme::bg);
    setColour(juce::TextEditor::textColourId, Theme::text);
    setColour(juce::TextEditor::highlightColourId, Theme::accent.withAlpha(0.4f));
    setColour(juce::TextEditor::outlineColourId, Theme::outline);
    setColour(juce::TextEditor::focusedOutlineColourId, Theme::accent);
    setColour(juce::Slider::rotarySliderFillColourId, Theme::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, Theme::control);
    setColour(juce::Slider::thumbColourId, Theme::text);
    setColour(juce::Slider::trackColourId, Theme::accent);
    setColour(juce::Slider::backgroundColourId, Theme::control);
    setColour(juce::Slider::textBoxTextColourId, Theme::text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, Theme::outline);
    setColour(juce::AlertWindow::backgroundColourId, Theme::panel);
    setColour(juce::AlertWindow::textColourId, Theme::text);
    setColour(juce::DocumentWindow::backgroundColourId, Theme::bg);
}

void DawLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                          bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto colour = background;
    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(0.4f);
    else if (down)
        colour = colour.brighter(0.15f);
    else if (highlighted)
        colour = colour.brighter(0.08f);

    g.setColour(colour);
    g.fillRoundedRectangle(bounds, 4.0f);
}

juce::Font DawLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return Theme::font(juce::jmin(13.0f, (float) buttonHeight * 0.55f), true);
}

void DawLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                                      float startAngle, float endAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(2.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const float lineW = juce::jmax(2.0f, radius * 0.2f);
    const float arcR = radius - lineW * 0.5f;

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
    g.strokePath(track, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar controls (pan, gain) fill from the centre.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float zeroPos = bipolar ? (float) slider.valueToProportionOfLength(0.0) : 0.0f;
    const float from = startAngle + zeroPos * (endAngle - startAngle);
    const float to = startAngle + pos * (endAngle - startAngle);

    if (std::abs(to - from) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin(from, to), juce::jmax(from, to), true);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(slider.isEnabled() ? 1.0f : 0.4f));
        g.strokePath(value, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const auto tip = centre.getPointOnCircumference(arcR * 0.65f, to);
    g.setColour(Theme::text);
    g.drawLine({ centre.getPointOnCircumference(arcR * 0.15f, to), tip }, 1.8f);
}

void DawLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                      juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat();
    if (slider.isBar())
    {
        // Value boxes like the tempo field: just a background behind the text.
        g.setColour(Theme::control);
        g.fillRoundedRectangle(slider.getLocalBounds().toFloat(), 4.0f);
        return;
    }
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto fill = slider.findColour(juce::Slider::trackColourId);

    if (vertical)
    {
        const float trackW = 4.0f;
        auto track = juce::Rectangle<float>(bounds.getCentreX() - trackW / 2, bounds.getY(), trackW, bounds.getHeight());
        g.setColour(slider.findColour(juce::Slider::backgroundColourId));
        g.fillRoundedRectangle(track, 2.0f);
        g.setColour(fill.withAlpha(0.8f));
        g.fillRoundedRectangle(track.withTop(pos), 2.0f);

        auto cap = juce::Rectangle<float>(bounds.getCentreX() - 11.0f, pos - 7.0f, 22.0f, 14.0f);
        g.setColour(Theme::panelLight.brighter(0.3f));
        g.fillRoundedRectangle(cap, 3.0f);
        g.setColour(Theme::text);
        g.drawLine(cap.getX() + 4, cap.getCentreY(), cap.getRight() - 4, cap.getCentreY(), 1.5f);
    }
    else
    {
        const float trackH = 4.0f;
        auto track = juce::Rectangle<float>(bounds.getX(), bounds.getCentreY() - trackH / 2, bounds.getWidth(), trackH);
        g.setColour(slider.findColour(juce::Slider::backgroundColourId));
        g.fillRoundedRectangle(track, 2.0f);
        g.setColour(fill.withAlpha(0.8f));
        g.fillRoundedRectangle(track.withRight(pos), 2.0f);
        g.setColour(Theme::text);
        g.fillEllipse(pos - 5.0f, bounds.getCentreY() - 5.0f, 10.0f, 10.0f);
    }
}

void DawLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(0.5f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, 4.0f);

    juce::Path arrow;
    const float ax = (float) w - 12.0f, ay = (float) h * 0.5f;
    arrow.startNewSubPath(ax - 3.5f, ay - 2.0f);
    arrow.lineTo(ax, ay + 2.0f);
    arrow.lineTo(ax + 3.5f, ay - 2.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(arrow, juce::PathStrokeType(1.5f));
}

void DawLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(4, 0, box.getWidth() - 20, box.getHeight());
    label.setFont(getComboBoxFont(box));
}
