#pragma once

#include "Effects.h"
#include <memory>

/**
    A reverb designer on CloudSeed's engine (MIT): choose a space (room, hall, plate, chamber,
    cathedral, ambience), then shape its pre-delay, size, decay, diffusion, early reflections,
    tone, movement and width. Shimmer feeds the tail back through a pitch shifter (an octave
    up, or a fifth, fourth or octave down), so each repeat rises: the shimmer sound.
*/
class ReverbEffect : public EffectPlugin
{
public:
    explicit ReverbEffect(te::PluginCreationInfo);
    ~ReverbEffect() override;

    static const char* getPluginName() { return "Reverb"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override;

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    enum Space { Room, Hall, Plate, Chamber, Cathedral, Ambience };
    static juce::StringArray spaceNames() { return { "Room", "Hall", "Plate", "Chamber", "Cathedral", "Ambience" }; }
    static juce::StringArray intervalNames() { return { "+12 Octave", "+7 Fifth", "+5 Fourth", "+19 Octave+5th", "+24 Two Octaves", "-12 Octave Down" }; }
    static float intervalSemitones(int index);

    te::AutomatableParameter *space, *preDelay, *size, *decay, *diffusion, *early;
    te::AutomatableParameter *lowCut, *highCut, *damping, *modDepth, *modRate, *width;
    te::AutomatableParameter *shimmer, *interval, *mix;

private:
    struct Engine;
    std::unique_ptr<Engine> cloud;
    void applySettings();
};
