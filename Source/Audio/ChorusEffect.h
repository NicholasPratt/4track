#pragma once

#include "Effects.h"
#include <memory>

/**
    Bucket-brigade choruses in three classic styles:

    - CE-2: one BBD swept by a triangle LFO, mixed with the dry signal: the mono pedal sound,
      with Rate and Depth.
    - Juno I, II and I+II: the Juno-60's stereo chorus, from the KR-106 emulation (GPL3),
      with its measured delays, rates, BBD filtering and hiss.
    - Dimension 1-4: two BBDs swept in opposite directions and cross-mixed, so the pitch
      wobble cancels and only width is left; the four buttons go from subtle to wide.
*/
class ChorusEffect : public EffectPlugin
{
public:
    explicit ChorusEffect(te::PluginCreationInfo);
    ~ChorusEffect() override;

    static const char* getPluginName() { return "Chorus"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return 0.05; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    bool isControlActive(const te::AutomatableParameter&) const override;

    enum Mode { CE2, JunoI, JunoII, JunoI_II, Dimension1, Dimension2, Dimension3, Dimension4 };
    static juce::StringArray modeNames()
    {
        return { "CE-2", "Juno I", "Juno II", "Juno I+II", "Dimension 1", "Dimension 2", "Dimension 3", "Dimension 4" };
    }

    te::AutomatableParameter *mode, *rate, *depth, *hiss, *mix;

private:
    struct Bbd;
    struct Juno;
    std::unique_ptr<Bbd> lineA, lineB;
    std::unique_ptr<Juno> juno;
    double lfoPhase = 0;
    int junoMode = -1;
};
