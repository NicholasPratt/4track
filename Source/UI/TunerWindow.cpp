#include "TunerWindow.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/TunerCapture.h"
#include "Theme.h"

namespace
{
    class TunerPanel final : public juce::Component, private juce::Timer
    {
    public:
        explicit TunerPanel(AudioEngine& audio) : engine(audio), capture(audio.getTracktion().getDeviceManager())
        {
            setSize(326, 176);
            timerCallback();
            startTimerHz(16);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(Theme::panel);
            auto area = getLocalBounds().reduced(18, 10);
            g.setColour(Theme::textDim);
            g.setFont(Theme::font(12));
            g.drawText(status, area.removeFromTop(17), juce::Justification::centred, true);

            const bool inTune = reading && std::abs(cents) <= 3.0;
            const auto pitchColour = inTune ? Theme::monitor : Theme::accent;
            g.setColour(reading ? pitchColour : Theme::textDim);
            g.setFont(Theme::font(35, true));
            g.drawText(note, area.removeFromTop(48), juce::Justification::centred);

            g.setColour(Theme::text);
            g.setFont(Theme::font(13));
            g.drawText(detail, area.removeFromTop(21), juce::Justification::centred);
            area.removeFromTop(8);

            const auto scale = area.removeFromTop(27).toFloat().reduced(12, 0);
            const auto centre = scale.getCentreX();
            g.setColour(Theme::control);
            g.fillRoundedRectangle(scale, 4);
            g.setColour(Theme::outline);
            for (int mark = -5; mark <= 5; ++mark)
            {
                const float x = centre + static_cast<float>(mark) * scale.getWidth() / 10;
                g.drawLine(x, scale.getY() + 7, x, scale.getBottom() - 7, 1.0f);
            }
            g.setColour(Theme::textDim);
            g.drawLine(centre, scale.getY() + 4, centre, scale.getBottom() - 4, 1.0f);
            if (reading)
            {
                const float x = centre + static_cast<float>(juce::jlimit(-50.0, 50.0, cents)) * scale.getWidth() / 100;
                g.setColour(pitchColour);
                g.fillRoundedRectangle(x - 2, scale.getY() + 2, 4, scale.getHeight() - 4, 2);
            }

            g.setColour(Theme::textDim);
            g.setFont(Theme::font(11));
            g.drawText("−50", area, juce::Justification::centredLeft);
            g.drawText("0", area, juce::Justification::centred);
            g.drawText("+50", area, juce::Justification::centredRight);
        }

    private:
        void timerCallback() override
        {
            const auto inputs = engine.getArmedInputChannels();
            TunerCapture::Channels channels;
            channels.fill(-1);
            for (size_t i = 0; i < juce::jmin(channels.size(), inputs.size()); ++i)
                channels[i] = inputs[i].channel;
            capture.setChannels(channels);
            reading = capture.readPitch();

            note = "—";
            detail = "A4 = 440 Hz";
            if (engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
                status = "No audio device";
            else if (inputs.empty())
                status = "Arm an audio track to tune";
            else
                status = "Play a single note";

            if (reading)
            {
                const auto source = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input)
                {
                    return input.channel == reading->channel;
                });
                if (source != inputs.end())
                    status = source->label;
                const auto pitch = 69.0 + 12.0 * std::log2(reading->frequency / 440.0);
                const int nearest = juce::roundToInt(pitch);
                cents = 100 * (pitch - nearest);
                note = juce::MidiMessage::getMidiNoteName(nearest, true, true, 4);
                const int roundedCents = juce::roundToInt(cents);
                detail = juce::String(reading->frequency, 1) + " Hz  ·  "
                    + (roundedCents > 0 ? "+" : "") + juce::String(roundedCents) + " cents";
            }
            repaint();
        }

        AudioEngine& engine;
        TunerCapture capture;
        std::optional<TunerCapture::Reading> reading;
        juce::String status, note, detail;
        double cents = 0;
    };

    class TunerWindow final : public juce::DocumentWindow
    {
    public:
        TunerWindow(AudioEngine& engine, juce::Rectangle<int> anchor)
            : DocumentWindow("Tuner", Theme::panel, closeButton)
        {
            setUsingNativeTitleBar(true);
            setAlwaysOnTop(true);
            setContentOwned(new TunerPanel(engine), true);
            centreAroundComponent(nullptr, getWidth(), getHeight());

            const auto& displays = juce::Desktop::getInstance().getDisplays();
            if (const auto* display = displays.getDisplayForRect(anchor))
            {
                const auto area = display->userBounds.toNearestInt();
                const auto x = juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - getWidth()),
                                            anchor.getCentreX() - getWidth() / 2);
                const auto y = juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - getHeight()),
                                            anchor.getBottom() + 30);
                setTopLeftPosition(x, y);
            }
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            clearContentComponent();
        }
    };
}

std::unique_ptr<juce::DocumentWindow> openTunerWindow(AudioEngine& engine, juce::Rectangle<int> anchor)
{
    return std::make_unique<TunerWindow>(engine, anchor);
}
