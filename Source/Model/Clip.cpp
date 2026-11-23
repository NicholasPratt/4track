#include "Clip.h"

void AudioSourceData::buildPeaks()
{
    const int n = audio.getNumSamples();
    const int numBlocks = (n + kPeakBlock - 1) / kPeakBlock;
    peakMin.assign((size_t) numBlocks, 0.0f);
    peakMax.assign((size_t) numBlocks, 0.0f);

    const float* l = audio.getReadPointer(0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer(1) : l;

    for (int b = 0; b < numBlocks; ++b)
    {
        const int s0 = b * kPeakBlock;
        const int s1 = juce::jmin(n, s0 + kPeakBlock);
        float lo = 0.0f, hi = 0.0f;
        for (int i = s0; i < s1; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            lo = juce::jmin(lo, v);
            hi = juce::jmax(hi, v);
        }
        peakMin[(size_t) b] = lo;
        peakMax[(size_t) b] = hi;
    }
}
