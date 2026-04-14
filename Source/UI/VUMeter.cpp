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
    // Smooth decay of display level for natural bar movement
    if (currentLevel > displayLevel)
    {
        displayLevel = currentLevel; // Instant attack
    }
    else
    {
        displayLevel *= decayRate; // Gradual decay
    }

    // Peak hold logic
    if (currentLevel > peakHold)
    {
        peakHold = currentLevel;
        peakHoldCounter = peakHoldFrames;
    }
    else if (peakHoldCounter > 0)
    {
        --peakHoldCounter;
    }
    else
    {
        peakHold *= 0.95f; // Slowly decay peak hold after hold time expires
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

    // Meter area (vertical bar)
    auto meterArea = bounds.reduced(4.0f);
    float meterWidth = meterArea.getWidth() * 0.6f;
    float meterHeight = meterArea.getHeight();

    // Center the meter horizontally
    float meterX = meterArea.getCentreX() - meterWidth / 2.0f;
    float meterY = meterArea.getY();

    // Draw meter background (dark grey)
    g.setColour(juce::Colour(0xff2a2a2a));
    g.fillRect(meterX, meterY, meterWidth, meterHeight);

    // Calculate level height
    float levelHeight = displayLevel * meterHeight;
    float levelY = meterY + meterHeight - levelHeight;

    // Draw level segments with color gradient
    // Green: 0-70% (-∞ to -6dB)
    // Yellow: 70-90% (-6dB to 0dB)
    // Red: 90-100% (0dB to +3dB)

    if (displayLevel > 0.0f)
    {
        // Green section (bottom 70%)
        float greenHeight = juce::jmin(levelHeight, meterHeight * 0.7f);
        if (greenHeight > 0.0f)
        {
            g.setColour(juce::Colour(0xff00ff00)); // Bright green
            g.fillRect(meterX, meterY + meterHeight - greenHeight, meterWidth, greenHeight);
        }

        // Yellow section (70-90%)
        if (displayLevel > 0.7f)
        {
            float yellowStart = meterY + meterHeight * 0.3f;
            float yellowHeight = juce::jmin(levelHeight - meterHeight * 0.7f, meterHeight * 0.2f);
            if (yellowHeight > 0.0f)
            {
                g.setColour(juce::Colour(0xffffff00)); // Yellow
                g.fillRect(meterX, yellowStart + meterHeight * 0.2f - yellowHeight, meterWidth, yellowHeight);
            }
        }

        // Red section (90-100%)
        if (displayLevel > 0.9f)
        {
            float redStart = meterY;
            float redHeight = levelHeight - meterHeight * 0.9f;
            if (redHeight > 0.0f)
            {
                g.setColour(juce::Colour(0xffff0000)); // Red
                g.fillRect(meterX, redStart + meterHeight * 0.1f - redHeight, meterWidth, redHeight);
            }
        }
    }

    // Draw peak hold indicator
    if (peakHold > 0.01f)
    {
        float peakY = meterY + meterHeight * (1.0f - peakHold);
        g.setColour(peakHold > 0.9f ? juce::Colours::red : juce::Colours::white);
        g.fillRect(meterX, peakY - 1.0f, meterWidth, 2.0f);
    }

    // Draw meter border
    g.setColour(juce::Colours::white);
    g.drawRect(meterX, meterY, meterWidth, meterHeight, 1.0f);

    // Draw dB scale markings on the right side
    g.setFont(juce::Font(8.0f));
    g.setColour(juce::Colours::white);

    // Draw scale: +3, 0, -6, -12, -18, -24, -∞
    std::vector<std::pair<juce::String, float>> scaleMarks = {
        {"+3", 1.0f},
        {"0", 0.9f},
        {"-6", 0.7f},
        {"-12", 0.5f},
        {"-18", 0.3f},
        {"-24", 0.15f},
        {"-\u221E", 0.0f}
    };

    float scaleX = meterX + meterWidth + 2.0f;
    for (const auto& mark : scaleMarks)
    {
        float y = meterY + meterHeight * (1.0f - mark.second);
        g.drawText(mark.first, scaleX, y - 6, 20, 12, juce::Justification::left);

        // Draw tick mark
        g.drawLine(meterX + meterWidth, y, meterX + meterWidth + 2, y, 1.0f);
    }
}

void VUMeter::resized()
{
    // Nothing to resize
}
