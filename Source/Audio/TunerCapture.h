#pragma once

#include "../Model/Arrangement.h"
#include <tracktion_engine/tracktion_engine.h>
#include <array>
#include <atomic>
#include <optional>
#include <span>

class TunerCapture final : private tracktion::DeviceManager::AudioInputObserver
{
public:
    static constexpr size_t maxChannels = kNumTracks * 2;
    static constexpr size_t analysisSize = 1536;
    using Channels = std::array<int, maxChannels>;

    explicit TunerCapture(tracktion::DeviceManager&);
    ~TunerCapture() override;

    struct Reading { int channel; double frequency; };
    void setChannels(const Channels&);
    std::optional<Reading> readPitch();

    static double frequencyOf(std::span<const float>, double sampleRate);

private:
    void onAudioInput(const float* const*, int channels, int frames, double sampleRate) override;

    struct Frame { std::array<float, maxChannels> samples {}; unsigned generation = 0; };
    static constexpr int fifoSize = 4096;
    tracktion::DeviceManager& devices;
    juce::AbstractFifo fifo { fifoSize };
    std::unique_ptr<Frame[]> queue = std::make_unique<Frame[]>(fifoSize);
    std::atomic<unsigned> generation { 0 };
    std::atomic<double> analysisRate { 0 };

    // The device manager's callback lock protects changes to the capture routing.
    Channels channels;
    std::array<float, maxChannels> sum {};
    int decimation = 1, accumulated = 0;
    double deviceRate = 0;

    // Consumer state, used only by the message thread.
    unsigned readGeneration = 0;
    size_t position = 0, available = 0;
    std::array<std::array<float, analysisSize>, maxChannels> history {};
};
