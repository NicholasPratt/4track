#pragma once

#include <tracktion_engine/tracktion_engine.h>

// The recording writer supplies consecutive blocks, including short blocks at
// punch boundaries. JUCE replaces a whole peak bucket on each addBlock call.
class RecordingThumbnail final : public juce::AudioThumbnail
{
public:
    RecordingThumbnail(juce::AudioFormatManager& formats, juce::AudioThumbnailCache& cache)
        : AudioThumbnail(bucketSize, formats, cache) {}

    void reset(int channels, double rate, juce::int64 length = 0) override
    {
        AudioThumbnail::reset(channels, rate, length);
        partial.setSize(channels, bucketSize);
        filled = 0;
    }

    void addBlock(juce::int64 position, const juce::AudioBuffer<float>& data, int offset, int count) override
    {
        if (filled > 0)
        {
            const int copied = juce::jmin(bucketSize - filled, count);
            copyPartial(data, offset, copied);
            if (filled < bucketSize)
                return;
            AudioThumbnail::addBlock(position + copied - bucketSize, partial, 0, bucketSize);
            filled = 0;
            position += copied;
            offset += copied;
            count -= copied;
        }

        const int complete = count - count % bucketSize;
        if (complete > 0)
            AudioThumbnail::addBlock(position, data, offset, complete);
        copyPartial(data, offset + complete, count - complete);
    }

private:
    // One bucket per pixel at the timeline's closest zoom.
    static constexpr int bucketSize = 8;
    juce::AudioBuffer<float> partial;
    int filled = 0;

    void copyPartial(const juce::AudioBuffer<float>& data, int offset, int count)
    {
        if (count == 0)
            return;
        for (int ch = 0; ch < partial.getNumChannels(); ++ch)
            partial.copyFrom(ch, filled, data, ch, offset, count);
        filled += count;
    }
};

class AudioUIBehaviour : public tracktion::UIBehaviour
{
public:
    std::unique_ptr<juce::AudioThumbnailBase> createAudioThumbnail(int resolution,
        juce::AudioFormatManager& formats, juce::AudioThumbnailCache& cache) override
    {
        // Tracktion requests 1024 for recording thumbnails and 256 for file thumbnails.
        if (resolution == 1024)
            return std::make_unique<RecordingThumbnail>(formats, cache);
        return UIBehaviour::createAudioThumbnail(resolution, formats, cache);
    }
};
