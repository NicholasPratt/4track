#include "Busy.h"
#include <atomic>
#include <thread>

namespace
{
    juce::Component::SafePointer<juce::Component> host;
    bool busy = false;

    constexpr double kShowAfterMs = 150.0;   // quicker jobs don't show the panel at all
    constexpr double kFadeMs = 150.0;

    /** Covers the window: dims it, swallows input, and draws the busy panel. */
    class Overlay : public juce::Component
    {
    public:
        Overlay(const juce::String& text, std::function<float()> getProgress)
            : message(text), progress(std::move(getProgress))
        {
            setWantsKeyboardFocus(true);
            setMouseCursor(juce::MouseCursor::WaitCursor);
        }

        /** Moves the animation on; elapsed is the time since the job started. */
        void update(double elapsedMs)
        {
            const float newAlpha = (float) juce::jlimit(0.0, 1.0, (elapsedMs - kShowAfterMs) / kFadeMs);
            angle = (float) (elapsedMs / 1000.0 * juce::MathConstants<double>::twoPi * 1.2);
            if (progress)
                fraction = juce::jlimit(0.0f, 1.0f, progress());
            if (newAlpha > 0.0f || alpha > 0.0f)
            {
                alpha = newAlpha;
                repaint();
            }
        }

        void showNow() { alpha = 1.0f; repaint(); }

        void paint(juce::Graphics& g) override
        {
            if (alpha <= 0.0f)
                return;
            g.fillAll(juce::Colours::black.withAlpha(0.45f * alpha));

            const bool hasBar = (bool) progress;
            auto panel = getLocalBounds().withSizeKeepingCentre(320, hasBar ? 104 : 84).toFloat();
            g.setColour(Theme::panelLight.withAlpha(alpha));
            g.fillRoundedRectangle(panel, 10.0f);
            g.setColour(Theme::outline.withAlpha(alpha));
            g.drawRoundedRectangle(panel.reduced(0.5f), 10.0f, 1.0f);

            auto content = panel.reduced(22.0f, 18.0f);
            auto top = content.removeFromTop(48.0f);

            // Spinner: a faint ring with a bright arc going round it.
            auto ring = top.removeFromLeft(48.0f).reduced(8.0f);
            const auto centre = ring.getCentre();
            const float radius = ring.getWidth() / 2.0f;
            g.setColour(Theme::outline.withAlpha(alpha));
            g.drawEllipse(ring, 3.5f);
            juce::Path arc;
            arc.addCentredArc(centre.x, centre.y, radius, radius, angle, 0.0f, juce::MathConstants<float>::pi * 0.65f, true);
            g.setColour(Theme::accent.withAlpha(alpha));
            g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            top.removeFromLeft(14.0f);
            g.setColour(Theme::text.withAlpha(alpha));
            g.setFont(Theme::font(15.0f, true));
            g.drawFittedText(message, top.toNearestInt(), juce::Justification::centredLeft, 2);

            if (hasBar)
            {
                auto bar = content.removeFromBottom(8.0f);
                g.setColour(Theme::bg.withAlpha(alpha));
                g.fillRoundedRectangle(bar, 4.0f);
                g.setColour(Theme::accent.withAlpha(alpha));
                g.fillRoundedRectangle(bar.withWidth(juce::jmax(bar.getHeight(), bar.getWidth() * fraction)), 4.0f);
            }
        }

        bool keyPressed(const juce::KeyPress&) override { return true; }   // nothing gets through while busy
        void inputAttemptWhenModal() override {}

    private:
        juce::String message;
        std::function<float()> progress;
        float alpha = 0.0f, angle = 0.0f, fraction = 0.0f;
    };

    std::unique_ptr<Overlay> showOverlay(const juce::String& message, std::function<float()> progress)
    {
        auto overlay = std::make_unique<Overlay>(message, std::move(progress));
        host->addAndMakeVisible(*overlay);
        overlay->setBounds(host->getLocalBounds());
        overlay->enterModalState(true);
        return overlay;
    }
}

namespace Busy
{
    void setHost(juce::Component* component) { host = component; }
    bool isBusy() { return busy; }

    void run(const juce::String& message, const std::function<void()>& work, std::function<float()> progress)
    {
        // Without a window (tests), or inside another job, just do the work.
        if (host == nullptr || busy || !juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            work();
            return;
        }

        busy = true;
        auto overlay = showOverlay(message, std::move(progress));

        std::atomic<bool> done { false };
        std::thread worker([&work, &done]
        {
            work();
            done = true;
        });

        // Keep the window painting (and the spinner turning) until the work is done.
        const double start = juce::Time::getMillisecondCounterHiRes();
        while (!done.load())
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
            overlay->update(juce::Time::getMillisecondCounterHiRes() - start);
        }
        worker.join();

        overlay->exitModalState(0);
        overlay.reset();
        busy = false;
    }

    void runHere(const juce::String& message, const std::function<void()>& work)
    {
        if (host == nullptr || busy)
        {
            work();
            return;
        }

        busy = true;
        auto overlay = showOverlay(message, {});
        overlay->showNow();
        if (auto* peer = host->getPeer())
            peer->performAnyPendingRepaintsNow();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(40);   // on screen before the message thread is tied up
        work();
        overlay->exitModalState(0);
        overlay.reset();
        busy = false;
    }
}
