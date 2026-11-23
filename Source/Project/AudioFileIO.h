#pragma once

#include "../Model/Clip.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace AudioFileIO
{
    /** Loads any supported audio file as a stereo source at targetRate (resampling if needed). */
    std::shared_ptr<AudioSourceData> loadSource(const juce::File& file, double targetRate, juce::String& error);

    /** Returns a copy of `source` resampled by outRate / inRate. */
    std::shared_ptr<AudioSourceData> resampled(const AudioSourceData& source, double inRate, double outRate);

    bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate,
                  int bitsPerSample, juce::String& error);
}
