#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace Mp3Encoder
{
    bool isAvailable();
    bool encode(const juce::File& wav, const juce::File& output, int bitrateKbps);
}
