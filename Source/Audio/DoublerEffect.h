#pragma once

#include "Effects.h"
#include <array>

/**
    A vocal doubler: makes a voice sound sung twice (or four times) by adding copies of it
    that come in a little late, a few cents off pitch, and drift in time the way a second
    take does. The lead stays where it is; the doubles are spread across the stereo field
    and can be darkened and thinned so they sit behind it.

    Each double is a delay line read by two taps that sweep and crossfade (a micro pitch
    shifter, like the classic studio harmonisers), with a slow random wander added to its
    delay. Different voices get different delays and opposite detunes, so in mono they don't
    comb-filter into one flanged copy.
*/
class DoublerEffect : public EffectPlugin
{
public:
    explicit DoublerEffect(te::PluginCreationInfo);
    ~DoublerEffect() override;

    static const char* getPluginName() { return "Doubler"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return kMaxSeconds; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    enum Voices { One, Two, Four };
    static juce::StringArray voiceNames() { return { "1 (Centre)", "2 (Left & Right)", "4 (Wide)" }; }
    static int voiceCount(int choice) { return choice == One ? 1 : choice == Two ? 2 : 4; }

    static constexpr int kMaxVoices = 4;
    static constexpr double kMaxSeconds = 0.16;

    te::AutomatableParameter *voices, *delay, *detune, *variation, *width, *lowCut, *highCut, *mix;

private:
    struct Voice
    {
        double shifterPhase = 0;     // 0..1: where the two taps are in their sweep
        double driftFrom = 0, driftTo = 0, driftPos = 1, driftRate = 1;   // the wander, -1..1
        juce::Random random;
    };
    std::array<Voice, kMaxVoices> voiceState;
    std::vector<float> line;
    int writePos = 0;
    double smoothedDelayMs = -1, smoothedDetune = -1, smoothedVariation = -1;
    EffectSvf lowCutFilter[2], highCutFilter[2];

    float read(double delaySamples) const;
};
