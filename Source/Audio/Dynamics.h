#pragma once

#include "Effects.h"

/**
    A compressor whose detector can listen to another track or a bus (the "key"), e.g. a
    kick ducking a bass. Tracktion routes the key track's output (after its fader) into
    two extra inputs; without a key, the compressor listens to what it compresses.
*/
class CompressorEffect : public KeyedEffect
{
public:
    explicit CompressorEffect(te::PluginCreationInfo);
    ~CompressorEffect() override;

    static const char* getPluginName() { return "Compressor"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int numInputs) override { return juce::jmin(numInputs, 2); }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    Display getDisplay() const override { return { "Reduction", reductionNow.load(std::memory_order_relaxed) / 24.0f }; }
    bool isControlActive(const te::AutomatableParameter&) const override;

    te::AutomatableParameter *threshold, *ratio, *knee, *attack, *release, *makeup, *mix;
    te::AutomatableParameter *detect, *keyFilter, *listen;

private:
    EffectSvf keyHp[2];
    float rms = 0, reduction = 0;   // reduction in dB, <= 0
    std::atomic<float> reductionNow { 0.0f };   // positive dB, for the display
};

/**
    A brickwall limiter for the master bus: nothing gets past the ceiling. It looks a few
    milliseconds ahead so it can turn down before a peak arrives, and with True Peak on it
    also catches the peaks between samples that a converter would make.
*/
class LimiterEffect : public EffectPlugin
{
public:
    explicit LimiterEffect(te::PluginCreationInfo);
    ~LimiterEffect() override;

    static const char* getPluginName() { return "Limiter"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getLatencySeconds() override { return lookahead / sampleRate; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    Display getDisplay() const override { return { "Reduction", reductionNow.load(std::memory_order_relaxed) / 12.0f }; }

    static constexpr double kLookaheadSeconds = 0.003;

    te::AutomatableParameter *gain, *ceiling, *release, *truePeak;

private:
    int lookahead = 1;
    juce::AudioBuffer<float> delay;        // the audio, waiting for the gain to catch up
    std::vector<float> wanted;             // gain each sample needs, over the lookahead
    std::vector<long long> minQueue;       // sample index of each entry in `wanted`: a sliding minimum
    int queueHead = 0, queueTail = 0;
    std::vector<float> smoothing;          // box filter over the held gain
    double smoothSum = 0;
    int pos = 0;
    long long sampleIndex = 0;
    float envelope = 1;
    float history[2][3] {};                // the last samples, for inter-sample peaks
    std::atomic<float> reductionNow { 0.0f };
};
