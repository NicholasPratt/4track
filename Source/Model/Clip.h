#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

/**
    Immutable block of audio shared by one or more clips (one per recorded take or
    imported file). Always stereo and always at the engine's current sample rate.
    Once published to the audio thread it is never modified.
*/
struct AudioSourceData
{
    // Where the audio lives on disk. Message-thread only, and updated when Save As
    // copies the file into a new project folder (hence mutable on a const source).
    mutable juce::File file;
    juce::AudioBuffer<float> audio;    // 2 channels

    // Waveform overview: min/max of the mono sum per kPeakBlock samples.
    static constexpr int kPeakBlock = 256;
    std::vector<float> peakMin, peakMax;

    juce::int64 getLength() const { return audio.getNumSamples(); }
    void buildPeaks();
};

using AudioSourcePtr = std::shared_ptr<const AudioSourceData>;

/** A region of an AudioSource placed on the timeline. Cheap to copy. */
struct Clip
{
    juce::int64 id = 0;
    AudioSourcePtr source;
    juce::int64 start = 0;         // timeline position, samples
    juce::int64 sourceOffset = 0;  // first sample of `source` that is heard
    juce::int64 length = 0;
    juce::int64 fadeIn = 0;
    juce::int64 fadeOut = 0;
    float gain = 1.0f;

    juce::int64 end() const { return start + length; }

    /** Gain from fades at a sample index relative to the clip start. */
    float fadeGainAt(juce::int64 rel) const
    {
        float g = 1.0f;
        if (fadeIn > 0 && rel < fadeIn)
            g = (float) rel / (float) fadeIn;
        const auto fromEnd = length - rel;
        if (fadeOut > 0 && fromEnd < fadeOut)
            g = juce::jmin(g, (float) fromEnd / (float) fadeOut);
        return g;
    }
};
