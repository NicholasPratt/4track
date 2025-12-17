#include "VUMeter.h"

VUMeter::VUMeter()
{
    startTimerHz(60); // 60 FPS for smooth needle animation
}

VUMeter::~VUMeter()
{
    stopTimer();
}

void VUMeter::setLevel(float newLevel)
{
    currentLevel = juce::jlimit(0.0f, 1.0f, newLevel);
}

void VUMeter::timerCallback()
{
    // Smooth decay of display level for natural needle movement
    if (currentLevel > displayLevel)
    {
        displayLevel = currentLevel; // Instant attack
    }
    else
    {
        displayLevel *= decayRate; // Gradual decay
    }

    repaint();
}

void VUMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Black background
    g.fillAll(juce::Colours::black);

    // White border
    g.setColour(juce::Colours::white);
    g.drawRect(bounds, 1.0f);

    // Draw meter scale markings
    auto meterArea = bounds.reduced(10.0f);
    float centerX = meterArea.getCentreX();
    float centerY = meterArea.getCentreY() + 10.0f;  // Pivot point in center
    float radius = meterArea.getWidth() * 0.4f;

    // Draw arc for meter range (-20 dB to +3 dB)
    // Adjust angles so needle doesn't dip below the meter box
    // -∞ dB (silence) should be at the left (-20 position)
    g.setColour(juce::Colours::white);
    float startAngle = juce::MathConstants<float>::pi * 1.0f;   // 180 degrees (straight left)
    float endAngle = 0.0f;                                       // 0 degrees (straight right)

    juce::Path arcPath;
    arcPath.addCentredArc(centerX, centerY, radius, radius,
                         0.0f, startAngle, endAngle, true);
    g.strokePath(arcPath, juce::PathStrokeType(1.5f));

    // Draw scale markings
    g.setColour(juce::Colours::white);
    for (int i = 0; i <= 10; ++i)
    {
        float t = i / 10.0f;
        float angle = startAngle + (endAngle - startAngle) * t;

        float x1 = centerX + std::cos(angle) * (radius - 5.0f);
        float y1 = centerY + std::sin(angle) * (radius - 5.0f);
        float x2 = centerX + std::cos(angle) * radius;
        float y2 = centerY + std::sin(angle) * radius;

        g.drawLine(x1, y1, x2, y2, (i % 5 == 0) ? 1.5f : 1.0f);
    }

    // Draw labels
    g.setFont(juce::Font(10.0f));
    g.drawText("-20", bounds.getX() + 5, centerY - 5, 20, 10,
               juce::Justification::centred);
    g.drawText("0", bounds.getCentreX() - 10, bounds.getY() + 5, 20, 10,
               juce::Justification::centred);
    g.drawText("+3", bounds.getRight() - 25, centerY - 5, 20, 10,
               juce::Justification::centred);

    // Draw the needle
    float needleAngle = startAngle + (endAngle - startAngle) * displayLevel;
    float needleLength = radius - 10.0f;

    float needleX = centerX + std::cos(needleAngle) * needleLength;
    float needleY = centerY + std::sin(needleAngle) * needleLength;

    // Needle in red if clipping (> 0.9), white otherwise
    if (displayLevel > 0.9f)
        g.setColour(juce::Colours::red);
    else
        g.setColour(juce::Colours::white);

    g.drawLine(centerX, centerY, needleX, needleY, 2.0f);

    // Draw center pivot
    g.fillEllipse(centerX - 3.0f, centerY - 3.0f, 6.0f, 6.0f);
}

void VUMeter::resized()
{
    // Nothing to resize
}
