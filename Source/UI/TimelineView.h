#pragma once

#include "../Audio/AudioEngine.h"
#include "Theme.h"
#include <cmath>

/** Horizontal mapping between timeline samples and pixels, shared by the ruler and the lanes. */
struct TimelineView
{
    double samplesPerPixel = 400.0;
    double viewStart = 0.0;   // sample at x = 0
    bool snapOn = true;

    float xFor(juce::int64 sample) const { return (float) (((double) sample - viewStart) / samplesPerPixel); }
    juce::int64 sampleAt(float x) const { return (juce::int64) std::llround(viewStart + (double) x * samplesPerPixel); }

    void zoomAround(float x, double factor)
    {
        const double anchor = viewStart + (double) x * samplesPerPixel;
        samplesPerPixel = juce::jlimit(8.0, 20000.0, samplesPerPixel * factor);
        viewStart = juce::jmax(0.0, anchor - (double) x * samplesPerPixel);
    }
};

/** Bars/beats grid for the current tempo and zoom level. */
struct Grid
{
    double samplesPerBeat = 22050.0;
    int beatsPerBar = 4;
    double step = 22050.0;   // grid and snap resolution in samples

    static Grid make(const AudioEngine& engine, const TimelineView& view)
    {
        Grid g;
        g.samplesPerBeat = engine.getSamplesPerBeat();
        g.beatsPerBar = juce::jmax(1, engine.beatsPerBar.load());

        const double pixelsPerBeat = g.samplesPerBeat / view.samplesPerPixel;
        if (pixelsPerBeat >= 96.0)      g.step = g.samplesPerBeat / 4.0;   // 16ths
        else if (pixelsPerBeat >= 48.0) g.step = g.samplesPerBeat / 2.0;   // 8ths
        else if (pixelsPerBeat >= 14.0) g.step = g.samplesPerBeat;         // beats
        else
        {
            double bars = 1.0;
            while (pixelsPerBeat * g.beatsPerBar * bars < 14.0)
                bars *= 2.0;
            g.step = g.samplesPerBeat * g.beatsPerBar * bars;
        }
        return g;
    }

    double samplesPerBar() const { return samplesPerBeat * beatsPerBar; }

    juce::int64 snap(juce::int64 s) const
    {
        return (juce::int64) std::llround(std::round((double) s / step) * step);
    }

    /** "bar.beat.sixteenth", 1-based, as shown on hardware and in most DAWs. */
    juce::String barsBeats(juce::int64 s) const
    {
        const bool negative = s < 0;
        const double beats = std::abs((double) s) / samplesPerBeat;
        const auto beatIndex = (juce::int64) std::floor(beats + 1e-9);
        const int sixteenth = (int) std::floor((beats - (double) beatIndex) * 4.0 + 1e-9);
        const auto bar = beatIndex / beatsPerBar;
        const auto beat = beatIndex % beatsPerBar;
        return (negative ? "-" : "") + juce::String(bar + 1) + "." + juce::String(beat + 1) + "." + juce::String(sixteenth + 1);
    }
};

inline juce::String formatTime(juce::int64 samples, double sampleRate)
{
    const bool negative = samples < 0;
    const double seconds = std::abs((double) samples) / juce::jmax(1.0, sampleRate);
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return (negative ? "-" : "") + juce::String(m) + ":" + juce::String(s, 3).paddedLeft('0', 6);
}
