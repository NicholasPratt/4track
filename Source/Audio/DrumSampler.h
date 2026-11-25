#pragma once

#include "Samples.h"
#include <array>

/**
    Sixteen pads, one sample each, played from MIDI notes 36 (C1) up: drop a sound on a pad
    and shape it with tune, attack, decay, tone, level and pan. Pads in the same choke group
    cut each other off (open and closed hi-hats). Like Sitala, it keeps the controls few.
*/
class DrumSampler : public EffectPlugin
{
public:
    explicit DrumSampler(te::PluginCreationInfo);
    ~DrumSampler() override;

    static const char* getPluginName() { return "Drum Sampler"; }
    static const char* xmlTypeName;
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    Kind getKind() const override { return Kind::Instrument; }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    double getTailLength() const override { return 1.0; }

    void initialise(const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void reset() override;
    void applyToBuffer(const te::PluginRenderContext&) override;
    void getFileReferences(std::vector<FileReference>&) override;
    void restorePluginStateFromValueTree(const juce::ValueTree&) override;

    static constexpr int kPads = 16, kFirstNote = 36, kVoices = 32;
    static juce::String rowFor(int pad) { return "Pad " + juce::String(pad + 1); }

    /** Loads a sound onto a pad (copied into the project); an empty file clears the pad. */
    bool loadPad(int pad, const juce::File&, juce::String& error);
    juce::File getPadFile(int pad) const;
    Sample::Ptr getPadSample(int pad) const { return slots[(size_t) pad].get(); }

    /** Plays a pad now (clicking it in the window), at this velocity. Any thread. */
    void audition(int pad, int velocity = 100);
    /** How recently each pad played (1 = just now), for lighting the pads. */
    float getActivity(int pad) const { return activity[(size_t) pad].load(); }

    struct Pad { te::AutomatableParameter *tune, *attack, *decay, *tone, *level, *pan, *choke; };
    std::array<Pad, kPads> pads;
    te::AutomatableParameter *velocity, *volume;

private:
    struct Voice
    {
        Sample::Ptr sample;
        int pad = -1;
        double pos = 0, step = 1;
        float gain = 0, panL = 1, panR = 1;
        juce::ADSR env;
        Tilt tilt[2];
        bool active = false;
        juce::uint32 started = 0;
    };
    void start(int pad, int velocity);
    void render(Voice&, float* l, float* r, int n);
    static juce::Identifier padProperty(int pad) { return juce::Identifier("pad" + juce::String(pad + 1)); }
    void reloadPad(int pad);

    std::array<SampleSlot, kPads> slots;
    std::array<Voice, kVoices> voices;
    std::array<std::atomic<float>, kPads> activity {};
    std::atomic<int> pendingAudition { -1 }, pendingVelocity { 100 };
    juce::uint32 counter = 0;
};
