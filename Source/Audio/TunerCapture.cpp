#include "TunerCapture.h"

#include <numeric>

namespace
{
    constexpr double minHz = 25, maxHz = 1600, targetRate = 12000;
    constexpr double minMeanSquare = 1e-6; // -60 dBFS
}

TunerCapture::TunerCapture(tracktion::DeviceManager& manager) : devices(manager)
{
    channels.fill(-1);
    devices.addAudioInputObserver(this);
}

TunerCapture::~TunerCapture()
{
    devices.removeAudioInputObserver(this);
}

void TunerCapture::setChannels(const Channels& next)
{
    if (next == channels)
        return;

    const juce::ScopedLock lock(devices.deviceManager.getAudioCallbackLock());
    channels = next;
    sum.fill(0);
    accumulated = 0;
    ++generation;
}

void TunerCapture::onAudioInput(const float* const* input, int numInputs, int frames, double rate)
{
    if (!juce::approximatelyEqual(rate, deviceRate))
    {
        deviceRate = rate;
        decimation = juce::jmax(1, juce::roundToInt(rate / targetRate));
        sum.fill(0);
        accumulated = 0;
        analysisRate = rate / decimation;
        ++generation;
    }

    if (channels.front() < 0)
        return;

    std::array<const float*, maxChannels> sources {};
    size_t count = 0;
    while (count < channels.size() && channels[count] >= 0)
    {
        const auto channel = channels[count];
        if (channel < numInputs)
            sources[count] = input[channel];
        ++count;
    }

    const int wanted = (accumulated + frames) / decimation;
    const auto write = fifo.write(wanted);
    const int capacity = write.blockSize1 + write.blockSize2;
    // A stalled reader can lose input. Start a new analysis window at the gap.
    if (capacity < wanted)
        ++generation;
    const auto currentGeneration = generation.load(std::memory_order_relaxed);
    int written = 0;
    for (int i = 0; i < frames; ++i)
    {
        for (size_t ch = 0; ch < count; ++ch)
            if (sources[ch] != nullptr)
                sum[ch] += sources[ch][i];

        if (++accumulated == decimation)
        {
            if (written < capacity)
            {
                const int index = written < write.blockSize1 ? write.startIndex1 + written
                                                              : write.startIndex2 + written - write.blockSize1;
                auto& frame = queue[static_cast<size_t>(index)];
                for (size_t ch = 0; ch < count; ++ch)
                    frame.samples[ch] = sum[ch] / static_cast<float>(decimation);
                frame.generation = currentGeneration;
                ++written;
            }
            sum.fill(0);
            accumulated = 0;
        }
    }
}

std::optional<TunerCapture::Reading> TunerCapture::readPitch()
{
    const auto currentGeneration = generation.load();
    if (currentGeneration != readGeneration)
    {
        readGeneration = currentGeneration;
        available = position = 0;
    }

    size_t count = 0;
    while (count < channels.size() && channels[count] >= 0)
        ++count;

    bool received = false;
    {
        const auto read = fifo.read(fifo.getNumReady());
        auto consume = [&](int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto& frame = queue[static_cast<size_t>(start + i)];
                if (frame.generation != readGeneration)
                    continue;

                for (size_t ch = 0; ch < count; ++ch)
                    history[ch][position] = frame.samples[ch];

                position = (position + 1) % analysisSize;
                available = juce::jmin(available + 1, analysisSize);
                received = true;
            }
        };
        consume(read.startIndex1, read.blockSize1);
        consume(read.startIndex2, read.blockSize2);
    }

    const double rate = analysisRate.load();
    if (!received || available < analysisSize || rate <= 0)
        return {};

    struct Level { size_t channel; double power; };
    std::array<Level, maxChannels> levels {};
    for (size_t ch = 0; ch < count; ++ch)
        levels[ch] = { ch, std::inner_product(history[ch].begin(), history[ch].end(),
                                              history[ch].begin(), 0.0) / analysisSize };
    std::sort(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(count),
              [](auto a, auto b) { return a.power > b.power; });

    std::array<float, analysisSize> samples;
    for (size_t i = 0; i < count && levels[i].power >= minMeanSquare; ++i)
    {
        const auto ch = levels[i].channel;
        std::rotate_copy(history[ch].begin(), history[ch].begin() + static_cast<std::ptrdiff_t>(position),
                         history[ch].end(), samples.begin());
        if (const auto frequency = frequencyOf(samples, rate); frequency > 0)
            return Reading { channels[ch], frequency };
    }
    return {};
}

double TunerCapture::frequencyOf(std::span<const float> signal, double rate)
{
    // YIN's cumulative mean normalized difference favours the first full period,
    // even when a harmonic is stronger than the fundamental.
    const int minLag = juce::jmax(2, static_cast<int>(std::floor(rate / maxHz)));
    const int maxLag = juce::jmin(static_cast<int>(analysisSize / 2), static_cast<int>(std::ceil(rate / minHz)));
    if (maxLag <= minLag || signal.size() < static_cast<size_t>(maxLag * 2))
        return 0;

    const size_t count = signal.size() - static_cast<size_t>(maxLag);
    double cumulative = 0;
    double previousDifference = 1, previousSquared = 0, earlierSquared = 0;
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double squared = 0;
        const auto* delayed = signal.data() + lag;
        for (size_t i = 0; i < count; ++i)
        {
            const auto delta = signal[i] - delayed[i];
            squared += delta * delta;
        }
        cumulative += squared;
        const double normalized = cumulative > 0 ? squared * lag / cumulative : 1;
        if (lag > minLag && previousDifference < 0.15 && normalized >= previousDifference)
        {
            // Interpolate the raw trough, where the normalization cannot skew the shape.
            const double curve = earlierSquared + squared - 2 * previousSquared;
            const double offset = curve > 0 ? juce::jlimit(-1.0, 1.0, 0.5 * (earlierSquared - squared) / curve) : 0;
            return rate / (lag - 1 + offset);
        }
        previousDifference = normalized;
        earlierSquared = previousSquared;
        previousSquared = squared;
    }
    return 0;
}
